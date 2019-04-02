#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>
#include <utility>

#include <node-addon-api/napi.h>

#include "src/converters.h"
#include "src/converters/macros.h"
#include "src/functional/maybe.h"
#include "src/functional/validation.h"

namespace node_webrtc {

#define DECLARE_TO_NAPI(T) DECLARE_CONVERTER(std::pair<Napi::Env COMMA T>, Napi::Value)

#define DECLARE_FROM_NAPI(T) DECLARE_CONVERTER(Napi::Value, T)

#define DECLARE_TO_AND_FROM_NAPI(T) \
  DECLARE_TO_NAPI(T) \
  DECLARE_FROM_NAPI(T)

#define TO_NAPI_IMPL(T, V) CONVERTER_IMPL(std::pair<Napi::Env COMMA T>, Napi::Value, V)

#define FROM_NAPI_IMPL(T, V) CONVERTER_IMPL(Napi::Value, T, V)

DECLARE_TO_AND_FROM_NAPI(bool)
DECLARE_TO_AND_FROM_NAPI(double)
DECLARE_TO_AND_FROM_NAPI(uint8_t)
DECLARE_TO_AND_FROM_NAPI(uint16_t)
DECLARE_TO_AND_FROM_NAPI(uint32_t)
DECLARE_TO_AND_FROM_NAPI(uint64_t)
DECLARE_TO_AND_FROM_NAPI(int8_t)
DECLARE_TO_AND_FROM_NAPI(int16_t)
DECLARE_TO_AND_FROM_NAPI(int32_t)
DECLARE_TO_AND_FROM_NAPI(int64_t)
DECLARE_TO_AND_FROM_NAPI(std::string)
DECLARE_FROM_NAPI(Napi::Function)
DECLARE_FROM_NAPI(Napi::Object)
DECLARE_TO_NAPI(std::vector<bool>)
DECLARE_FROM_NAPI(Napi::ArrayBuffer)

template <typename T>
struct Converter<Napi::Value, Maybe<T>> {
  static Validation<Maybe<T>> Convert(const Napi::Value value) {
    return value.IsUndefined()
        ? Pure(MakeNothing<T>())
        : From<T>(value).Map(&MakeJust<T>);
  }
};

template <typename T>
struct Converter<std::pair<Napi::Env, Maybe<T>>, Napi::Value> {
  static Validation<Napi::Value> Convert(const std::pair<Napi::Env, Maybe<T>> pair) {
    return pair.second.IsJust()
        ? From<Napi::Value>(std::make_pair(pair.first, pair.second.UnsafeFromJust()))
        : Pure(pair.first.Null());
  }
};

template <>
struct Converter<Napi::Value, Napi::Array> {
  static Validation<Napi::Array> Convert(Napi::Value value) {
    return value.IsArray()
        ? Pure(value.As<Napi::Array>())
        : Validation<Napi::Array>::Invalid(("Expected an array"));
  }
};

template <typename T>
struct Converter<Napi::Array, std::vector<T>> {
  static Validation<std::vector<T>> Convert(const Napi::Array array) {
    auto validated = std::vector<T>();
    validated.reserve(array.Length());
    for (uint32_t i = 0; i < array.Length(); i++) {
      auto maybeValue = array.Get(i);
      if (maybeValue.Env().IsExceptionPending()) {
        return Validation<std::vector<T>>::Invalid(maybeValue.Env().GetAndClearPendingException().Message());
      }
      auto maybeValidated = From<T>(maybeValue);
      if (maybeValidated.IsInvalid()) {
        return Validation<std::vector<T>>::Invalid(maybeValidated.ToErrors());
      }
      validated.push_back(maybeValidated.UnsafeFromValid());
    }
    return validated;
  }
};

template <typename T>
struct Converter<std::pair<Napi::Env, std::vector<T>>, Napi::Value> {
  static Validation<Napi::Value> Convert(std::pair<Napi::Env, std::vector<T>> pair) {
    Napi::EscapableHandleScope scope(pair.first);
    auto maybeArray = Napi::Array::New(pair.first, pair.second.size());
    uint32_t i = 0;
    for (const auto& value : pair.second) {
      auto maybeValue = From<Napi::Value>(value);
      if (maybeValue.IsInvalid()) {
        return Validation<Napi::Value>::Invalid(maybeValue.ToErrors());
      }
      maybeArray.Set(i++, maybeValue.UnsafeFromValid());
      if (maybeArray.Env().IsExceptionPending()) {
        return Validation<Napi::Value>::Invalid(maybeArray.Env().GetAndClearPendingException());
      }
    }
    return Pure(scope.Escape(maybeArray));
  }
};

}  // namespace node_webrtc
