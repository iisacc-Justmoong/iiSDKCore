#pragma once
#include <iiSDKCore/Core/Result/Result.h>
#include <iiSDKCore/Core/Text/Text.h>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ii::runtime::detail {
// CommandLineToArgvW supplies UTF-16 code units. Copying avoids assuming that
// wchar_t and char16_t have compatible aliasing or identical representation.
inline core::Result<std::vector<std::string>> decode_windows_arguments(std::span<wchar_t* const> arguments) {
    std::vector<std::string> decoded;
    decoded.reserve(arguments.size());
    for (const auto* argument : arguments) {
        if (!argument) return core::fail(core::ErrorCode::invalid_argument, "Null Windows command-line argument");
        std::u16string units;
        for (auto current = argument; *current; ++current) {
            const auto unit = static_cast<std::uint32_t>(*current);
            if (unit > 0xffff) return core::fail(core::ErrorCode::invalid_argument, "Invalid Windows UTF-16 code unit");
            units.push_back(static_cast<char16_t>(unit));
        }
        auto utf8 = core::to_utf8(units);
        if (!utf8) return core::fail(core::ErrorCode::invalid_argument, "Invalid UTF-16 in Windows command-line argument");
        decoded.push_back(std::move(*utf8));
    }
    return decoded;
}
} // namespace ii::runtime::detail
