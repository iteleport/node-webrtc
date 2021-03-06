/* Copyright (c) 2019 The node-webrtc project authors. All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be found
 * in the LICENSE.md file in the root of the source tree. All contributing
 * project authors may be found in the AUTHORS file in the root of the source
 * tree.
 */
#pragma once

#include <iosfwd>
#include <map>
#include <string>

#include <nan.h>

namespace v8 { class Function; }
namespace v8 { class Object; }
namespace v8 { template <class T> class Local; }

namespace node_webrtc {

class LegacyStatsReport
  : public Nan::ObjectWrap {
 public:
  LegacyStatsReport() = delete;

  ~LegacyStatsReport() override = default;

  //
  // Nodejs wrapping.
  //
  static void Init(v8::Handle<v8::Object> exports);

  static LegacyStatsReport* Create(double timestamp, const std::map<std::string, std::string>& stats);

 private:
  explicit LegacyStatsReport(double timestamp, const std::map<std::string, std::string>& stats)
    : _timestamp(timestamp), _stats(stats) {}

  static Nan::Persistent<v8::Function>& constructor();

  static NAN_METHOD(New);

  static NAN_METHOD(names);
  static NAN_METHOD(stat);

  static NAN_GETTER(GetTimestamp);
  static NAN_GETTER(GetType);

  static NAN_SETTER(ReadOnly);

  double _timestamp;
  std::map<std::string, std::string> _stats;
};

}  // namespace node_webrtc
