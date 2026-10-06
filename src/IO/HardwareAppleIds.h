#pragma once
#include <bit>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace ii::io::detail {
// CoreMIDI UniqueID is SInt32. Accept the former unsigned spelling as an alias.
inline std::optional<std::int32_t> core_midi_identity(std::string_view id) {
  if (!id.starts_with("midi:"))
    return {};
  id.remove_prefix(5);
  std::int64_t value{};
  const auto result = std::from_chars(id.data(), id.data() + id.size(), value);
  if (result.ec != std::errc{} || result.ptr != id.data() + id.size() ||
      value < std::numeric_limits<std::int32_t>::min() ||
      value > std::numeric_limits<std::uint32_t>::max())
    return {};
  if (value < 0)
    return static_cast<std::int32_t>(value);
  return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}
} // namespace ii::io::detail
