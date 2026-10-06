#pragma once
#include "../Memory/SharedValue.h"
#include "Serialization.h"

namespace ii::serialization {
// Process-local ownership, locks and revisions never enter the wire format.
template <class T>
  requires Serializable<T> && std::copy_constructible<T>
struct Codec<ii::core::SharedValue<T>> final {
  using Shared = ii::core::SharedValue<T>;
  static Result<void> encode(Encoder &out, const Shared &value) {
    auto snapshot = value.snapshot();
    return out.write(snapshot.get());
  }
  static Result<Shared> decode(Decoder &in) {
    auto value = in.read<T>();
    if (!value)
      return std::unexpected(value.error());
    if (auto result = in.allocate(1, sizeof(T)); !result)
      return std::unexpected(result.error());
    return Result<Shared>{std::in_place, std::move(*value)};
  }
};
} // namespace ii::serialization
