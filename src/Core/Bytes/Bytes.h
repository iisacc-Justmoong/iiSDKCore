#pragma once
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace ii::core {
using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;
// Borrowed bytes; the source must outlive the view. No text encoding conversion.
inline ByteView byte_view(std::string_view value) noexcept {
    return std::as_bytes(std::span(value.data(), value.size()));
}
} // namespace ii::core
