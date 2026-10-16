#pragma once
#include "../Bytes/Bytes.h"

namespace ii::core {
// Unicode scalar UTF-8: reject overlong encodings, surrogates and truncation.
// NUL is valid text; path and protocol boundaries impose their own restrictions.
inline bool valid_utf8(ByteView bytes) noexcept {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto first = std::to_integer<unsigned>(bytes[i++]);
        if (first < 0x80) continue;
        unsigned count = 0, value = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; value = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; value = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; value = first & 7; minimum = 0x10000; }
        else return false;
        if (count > bytes.size() - i) return false;
        while (count--) {
            const auto next = std::to_integer<unsigned>(bytes[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
inline bool valid_utf8(std::string_view text) noexcept { return valid_utf8(byte_view(text)); }
} // namespace ii::core
