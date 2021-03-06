/* Copyright (c) 2019 The node-webrtc project authors. All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be found
 * in the LICENSE.md file in the root of the source tree. All contributing
 * project authors may be found in the AUTHORS file in the root of the source
 * tree.
 */
#include "src/interfaces/rtc_data_channel.h"

#include <utility>

#include <v8.h>
#include <webrtc/api/data_channel_interface.h>
#include <webrtc/api/scoped_refptr.h>
#include <webrtc/rtc_base/copy_on_write_buffer.h>

#include "src/enums/webrtc/data_state.h"
#include "src/node/error_factory.h"
#include "src/node/events.h"

namespace node_webrtc {

Nan::Persistent<v8::Function>& RTCDataChannel::constructor() {
  static Nan::Persistent<v8::Function> constructor;
  return constructor;
}

DataChannelObserver::DataChannelObserver(std::shared_ptr<PeerConnectionFactory> factory,
    rtc::scoped_refptr<webrtc::DataChannelInterface> jingleDataChannel):
  _factory(std::move(factory))
  , _jingleDataChannel(std::move(jingleDataChannel)) {
  _jingleDataChannel->RegisterObserver(this);
}

void DataChannelObserver::OnStateChange() {
  auto state = _jingleDataChannel->state();
  Enqueue(Callback1<RTCDataChannel>::Create([state](RTCDataChannel & channel) {
    RTCDataChannel::HandleStateChange(channel, state);
  }));
}

void DataChannelObserver::OnMessage(const webrtc::DataBuffer& buffer) {
  Enqueue(Callback1<RTCDataChannel>::Create([buffer](RTCDataChannel & channel) {
    RTCDataChannel::HandleMessage(channel, buffer);
  }));
}

static void requeue(DataChannelObserver& observer, RTCDataChannel& channel) {
  while (auto event = observer.Dequeue()) {
    channel.Dispatch(std::move(event));
  }
}

RTCDataChannel::RTCDataChannel(node_webrtc::DataChannelObserver* observer)
  : AsyncObjectWrapWithLoop<RTCDataChannel>("RTCDataChannel", *this)
  , _binaryType(BinaryType::kArrayBuffer)
  , _factory(observer->_factory)
  , _jingleDataChannel(observer->_jingleDataChannel) {
  _jingleDataChannel->RegisterObserver(this);

  // Re-queue cached observer events
  requeue(*observer, *this);

  delete observer;

  // NOTE(mroberts): These doesn't actually matter yet.
  _cached_id = 0;
  _cached_max_packet_life_time = 0;
  _cached_max_retransmits = 0;
  _cached_negotiated = false;
  _cached_ordered = false;
  _cached_buffered_amount = 0;
}

RTCDataChannel::~RTCDataChannel() {
  wrap()->Release(this);
}

void RTCDataChannel::CleanupInternals() {
  if (_jingleDataChannel == nullptr) {
    return;
  }
  _jingleDataChannel->UnregisterObserver();
  _cached_id = _jingleDataChannel->id();
  _cached_label = _jingleDataChannel->label();
  _cached_max_packet_life_time = _jingleDataChannel->maxRetransmitTime();
  _cached_max_retransmits = _jingleDataChannel->maxRetransmits();
  _cached_negotiated = _jingleDataChannel->negotiated();
  _cached_ordered = _jingleDataChannel->ordered();
  _cached_protocol = _jingleDataChannel->protocol();
  _cached_buffered_amount = _jingleDataChannel->buffered_amount();
  _jingleDataChannel = nullptr;
}

void RTCDataChannel::OnPeerConnectionClosed() {
  if (_jingleDataChannel != nullptr) {
    CleanupInternals();
    Stop();
  }
}

NAN_METHOD(RTCDataChannel::New) {
  if (!info.IsConstructCall()) {
    return Nan::ThrowTypeError("Use the new operator to construct the RTCDataChannel.");
  }

  v8::Local<v8::External> _observer = v8::Local<v8::External>::Cast(info[0]);
  auto observer = static_cast<node_webrtc::DataChannelObserver*>(_observer->Value());

  auto obj = new RTCDataChannel(observer);
  obj->Wrap(info.This());

  info.GetReturnValue().Set(info.This());
}

void RTCDataChannel::OnStateChange() {
  auto state = _jingleDataChannel->state();
  if (state == webrtc::DataChannelInterface::kClosed) {
    CleanupInternals();
  }
  Dispatch(CreateCallback<RTCDataChannel>([this, state]() {
    RTCDataChannel::HandleStateChange(*this, state);
  }));
}

void RTCDataChannel::HandleStateChange(RTCDataChannel& channel, webrtc::DataChannelInterface::DataState state) {
  Nan::HandleScope scope;
  v8::Local<v8::Value> argv[1];
  if (state == webrtc::DataChannelInterface::kClosed) {
    argv[0] = Nan::New("closed").ToLocalChecked();
    channel.MakeCallback("onstatechange", 1, argv);
  } else if (state == webrtc::DataChannelInterface::kOpen) {
    argv[0] = Nan::New("open").ToLocalChecked();
    channel.MakeCallback("onstatechange", 1, argv);
  }
  if (state == webrtc::DataChannelInterface::kClosed) {
    channel.Stop();
  }
}

void RTCDataChannel::OnMessage(const webrtc::DataBuffer& buffer) {
  Dispatch(CreateCallback<RTCDataChannel>([this, buffer]() {
    RTCDataChannel::HandleMessage(*this, buffer);
  }));
}

void RTCDataChannel::HandleMessage(RTCDataChannel& channel, const webrtc::DataBuffer& buffer) {
  bool binary = buffer.binary;
  size_t size = buffer.size();
  std::unique_ptr<char[]> message = std::unique_ptr<char[]>(new char[size]);
  memcpy(reinterpret_cast<void*>(message.get()), reinterpret_cast<const void*>(buffer.data.data()), size);

  Nan::HandleScope scope;
  v8::Local<v8::Value> argv[1];
  if (binary) {
    v8::Local<v8::ArrayBuffer> array = v8::ArrayBuffer::New(
            v8::Isolate::GetCurrent(),
            message.release(),
            size,
            v8::ArrayBufferCreationMode::kInternalized);
    argv[0] = array;
  } else {
    v8::Local<v8::String> str = Nan::New(message.get(), size).ToLocalChecked();
    argv[0] = str;
  }
  channel.MakeCallback("onmessage", 1, argv);
}

NAN_METHOD(RTCDataChannel::Send) {
  auto self = AsyncObjectWrapWithLoop<RTCDataChannel>::Unwrap(info.This());

  if (self->_jingleDataChannel != nullptr) {
    if (self->_jingleDataChannel->state() != webrtc::DataChannelInterface::DataState::kOpen) {
      return Nan::ThrowError(ErrorFactory::CreateInvalidStateError("RTCDataChannel.readyState is not 'open'"));
    }
    if (info[0]->IsString()) {
      v8::Local<v8::String> str = v8::Local<v8::String>::Cast(info[0]);
      std::string data = *v8::String::Utf8Value(str);

      webrtc::DataBuffer buffer(data);
      self->_jingleDataChannel->Send(buffer);
    } else {
      v8::Local<v8::ArrayBuffer> arraybuffer;
      size_t byte_offset = 0;
      size_t byte_length = 0;

      if (info[0]->IsArrayBufferView()) {
        v8::Local<v8::ArrayBufferView> view = v8::Local<v8::ArrayBufferView>::Cast(info[0]);
        arraybuffer = view->Buffer();
        byte_offset = view->ByteOffset();
        byte_length = view->ByteLength();
      } else if (info[0]->IsArrayBuffer()) {
        arraybuffer = v8::Local<v8::ArrayBuffer>::Cast(info[0]);
        byte_length = arraybuffer->ByteLength();
      } else {
        return Nan::ThrowTypeError("Expected a Blob or ArrayBuffer");
      }

      v8::ArrayBuffer::Contents content = arraybuffer->GetContents();
      rtc::CopyOnWriteBuffer buffer(
          static_cast<char*>(content.Data()) + byte_offset,
          byte_length);

      webrtc::DataBuffer data_buffer(buffer, true);
      self->_jingleDataChannel->Send(data_buffer);
    }
  } else {
    return Nan::ThrowError(ErrorFactory::CreateInvalidStateError("RTCDataChannel.readyState is not 'open'"));
  }
}

NAN_METHOD(RTCDataChannel::Close) {
  auto self = AsyncObjectWrapWithLoop<RTCDataChannel>::Unwrap(info.This());

  if (self->_jingleDataChannel != nullptr) {
    self->_jingleDataChannel->Close();
  }
}

NAN_GETTER(RTCDataChannel::GetBufferedAmount) {
  (void) property;

  auto self = AsyncObjectWrapWithLoop<RTCDataChannel>::Unwrap(info.Holder());

  uint64_t buffered_amount = self->_jingleDataChannel != nullptr
      ? self->_jingleDataChannel->buffered_amount()
      : self->_cached_buffered_amount;

  info.GetReturnValue().Set(Nan::New<v8::Number>(buffered_amount));
}

NAN_GETTER(RTCDataChannel::GetId) {
  (void) property;

  auto self = AsyncObjectWrapWithLoop<RTCDataChannel>::Unwrap(info.Holder());

  auto id = self->_jingleDataChannel
      ? self->_jingleDataChannel->id()
      : self->_cached_id;

  info.GetReturnValue().Set(Nan::New<v8::Number>(id));
}

NAN_GETTER(RTCDataChannel::GetLabel) {
  (void) property;

  auto self = AsyncObjectWrapWithLoop<RTCDataChannel>::Unwrap(info.Holder());

  std::string label = self->_jingleDataChannel != nullptr
      ? self->_jingleDataChannel->label()
      : self->_cached_label;

  info.GetReturnValue().Set(Nan::New(label).ToLocalChecked());
}

NAN_GETTER(RTCDataChannel::GetMaxPacketLifeTime) {
  (void) property;

  auto self = AsyncObjectWrapWithLoop<RTCDataChannel>::Unwrap(info.Holder());

  auto max_packet_life_time = self->_jingleDataChannel
      ? self->_jingleDataChannel->maxRetransmitTime()
      : self->_cached_max_packet_life_time;

  info.GetReturnValue().Set(Nan::New(max_packet_life_time));
}

NAN_GETTER(RTCDataChannel::GetMaxRetransmits) {
  (void) property;

  auto self = AsyncObjectWrapWithLoop<RTCDataChannel>::Unwrap(info.Holder());

  auto max_retransmits = self->_jingleDataChannel
      ? self->_jingleDataChannel->maxRetransmits()
      : self->_cached_max_retransmits;

  info.GetReturnValue().Set(Nan::New(max_retransmits));
}

NAN_GETTER(RTCDataChannel::GetNegotiated) {
  (void) property;

  auto self = AsyncObjectWrapWithLoop<RTCDataChannel>::Unwrap(info.Holder());

  auto negotiated = self->_jingleDataChannel
      ? self->_jingleDataChannel->negotiated()
      : self->_cached_negotiated;

  info.GetReturnValue().Set(negotiated);
}

NAN_GETTER(RTCDataChannel::GetOrdered) {
  (void) property;

  auto self = AsyncObjectWrapWithLoop<RTCDataChannel>::Unwrap(info.Holder());

  auto ordered = self->_jingleDataChannel
      ? self->_jingleDataChannel->ordered()
      : self->_cached_ordered;

  info.GetReturnValue().Set(Nan::New(ordered));
}

NAN_GETTER(RTCDataChannel::GetPriority) {
  (void) property;

  info.GetReturnValue().Set(Nan::New("high").ToLocalChecked());
}

NAN_GETTER(RTCDataChannel::GetProtocol) {
  (void) property;

  auto self = AsyncObjectWrapWithLoop<RTCDataChannel>::Unwrap(info.Holder());

  auto protocol = self->_jingleDataChannel
      ? self->_jingleDataChannel->protocol()
      : self->_cached_protocol;

  info.GetReturnValue().Set(Nan::New(protocol).ToLocalChecked());
}

NAN_GETTER(RTCDataChannel::GetReadyState) {
  (void) property;

  auto self = AsyncObjectWrapWithLoop<RTCDataChannel>::Unwrap(info.Holder());

  CONVERT_OR_THROW_AND_RETURN(self->_jingleDataChannel
      ? self->_jingleDataChannel->state()
      : webrtc::DataChannelInterface::kClosed,
      state,
      v8::Local<v8::Value>)

  info.GetReturnValue().Set(state);
}

NAN_GETTER(RTCDataChannel::GetBinaryType) {
  (void) property;

  auto self = AsyncObjectWrapWithLoop<RTCDataChannel>::Unwrap(info.Holder());

  CONVERT_OR_THROW_AND_RETURN(self->_binaryType, binaryType, v8::Local<v8::Value>)

  info.GetReturnValue().Set(binaryType);
}

NAN_SETTER(RTCDataChannel::SetBinaryType) {
  (void) property;

  auto self = AsyncObjectWrapWithLoop<RTCDataChannel>::Unwrap(info.Holder());

  CONVERT_OR_THROW_AND_RETURN(value, binaryType, BinaryType)

  self->_binaryType = binaryType;
}

Wrap <
RTCDataChannel*,
rtc::scoped_refptr<webrtc::DataChannelInterface>,
node_webrtc::DataChannelObserver*
> * RTCDataChannel::wrap() {
  static auto wrap = new node_webrtc::Wrap <
  RTCDataChannel*,
  rtc::scoped_refptr<webrtc::DataChannelInterface>,
  node_webrtc::DataChannelObserver*
  > (RTCDataChannel::Create);
  return wrap;
}

RTCDataChannel* RTCDataChannel::Create(
    node_webrtc::DataChannelObserver* observer,
    rtc::scoped_refptr<webrtc::DataChannelInterface>) {
  Nan::HandleScope scope;
  v8::Local<v8::Value> cargv = Nan::New<v8::External>(static_cast<void*>(observer));
  auto channel = Nan::NewInstance(Nan::New(RTCDataChannel::constructor()), 1, &cargv).ToLocalChecked();
  return AsyncObjectWrapWithLoop<RTCDataChannel>::Unwrap(channel);
}

void RTCDataChannel::Init(v8::Handle<v8::Object> exports) {
  v8::Local<v8::FunctionTemplate> tpl = Nan::New<v8::FunctionTemplate>(New);
  tpl->SetClassName(Nan::New("RTCDataChannel").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  Nan::SetPrototypeMethod(tpl, "close", Close);
  Nan::SetPrototypeMethod(tpl, "send", Send);

  Nan::SetAccessor(tpl->InstanceTemplate(), Nan::New("bufferedAmount").ToLocalChecked(), GetBufferedAmount, nullptr);
  Nan::SetAccessor(tpl->InstanceTemplate(), Nan::New("id").ToLocalChecked(), GetId, nullptr);
  Nan::SetAccessor(tpl->InstanceTemplate(), Nan::New("label").ToLocalChecked(), GetLabel, nullptr);
  Nan::SetAccessor(tpl->InstanceTemplate(), Nan::New("maxPacketLifeTime").ToLocalChecked(), GetMaxPacketLifeTime, nullptr);
  Nan::SetAccessor(tpl->InstanceTemplate(), Nan::New("maxRetransmits").ToLocalChecked(), GetMaxRetransmits, nullptr);
  Nan::SetAccessor(tpl->InstanceTemplate(), Nan::New("negotiated").ToLocalChecked(), GetNegotiated, nullptr);
  Nan::SetAccessor(tpl->InstanceTemplate(), Nan::New("ordered").ToLocalChecked(), GetOrdered, nullptr);
  Nan::SetAccessor(tpl->InstanceTemplate(), Nan::New("priority").ToLocalChecked(), GetPriority, nullptr);
  Nan::SetAccessor(tpl->InstanceTemplate(), Nan::New("protocol").ToLocalChecked(), GetProtocol, nullptr);
  Nan::SetAccessor(tpl->InstanceTemplate(), Nan::New("binaryType").ToLocalChecked(), GetBinaryType, SetBinaryType);
  Nan::SetAccessor(tpl->InstanceTemplate(), Nan::New("readyState").ToLocalChecked(), GetReadyState, nullptr);

  constructor().Reset(tpl->GetFunction());
  exports->Set(Nan::New("RTCDataChannel").ToLocalChecked(), tpl->GetFunction());
}

}  // namespace node_webrtc
