#pragma once
#include <iiSDKCore/Core/Serialization/Serialization.h>
struct StoredSample final {
  std::uint64_t sequence;
  std::string label;
  std::vector<float> samples;
  bool operator==(const StoredSample &) const = default;
};
namespace ii::serialization {
template <> struct Codec<StoredSample> final {
  static Result<void> encode(Encoder &out, const StoredSample &value) {
    if (auto r = out.array(4); !r)
      return r;
    if (auto r = out.write(std::uint32_t{1}); !r)
      return r;
    if (auto r = out.write(value.sequence); !r)
      return r;
    if (auto r = out.write(value.label); !r)
      return r;
    return out.write(value.samples);
  }
  static Result<StoredSample> decode(Decoder &in) {
    auto size = in.array();
    if (!size)
      return std::unexpected(size.error());
    auto version = in.read<std::uint32_t>();
    if (!version)
      return std::unexpected(version.error());
    if ((*version == 1 && *size != 4) || (*version == 0 && *size != 3) ||
        *version > 1)
      return in.reject<StoredSample>(ErrorCode::schema_mismatch,
                                     "Unsupported sample schema");
    auto sequence = in.read<std::uint64_t>();
    if (!sequence)
      return std::unexpected(sequence.error());
    std::string label = "legacy";
    if (*version == 1) {
      auto field = in.read<std::string>();
      if (!field)
        return std::unexpected(field.error());
      label = std::move(*field);
    }
    auto samples = in.read<std::vector<float>>();
    if (!samples)
      return std::unexpected(samples.error());
    return StoredSample{*sequence, std::move(label), std::move(*samples)};
  }
};
} // namespace ii::serialization
