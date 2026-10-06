#pragma once
#include "../Result/Result.h"
#include <charconv>
#include <compare>
#include <cstdint>
#include <string_view>

namespace ii::core {
// Numeric SDK/API version. Prerelease/build identifiers are intentionally absent.
struct Version final {
    std::uint32_t major=0,minor=0,patch=0;
    auto operator<=>(const Version&) const = default;
    [[nodiscard]] std::string string() const { return std::to_string(major)+"."+std::to_string(minor)+"."+std::to_string(patch); }
    [[nodiscard]] static Result<Version> parse(std::string_view input) {
        Version result;
        std::uint32_t* fields[]={&result.major,&result.minor,&result.patch};
        for (std::size_t i=0;i<3;++i) {
            const auto dot=input.find('.');
            if ((i<2 && dot==input.npos) || (i==2 && dot!=input.npos)) return fail(ErrorCode::invalid_argument,"Expected major.minor.patch");
            auto part=input.substr(0,dot);
            if (part.empty() || (part.size()>1 && part[0]=='0')) return fail(ErrorCode::invalid_argument,"Invalid version component");
            auto parsed=std::from_chars(part.data(),part.data()+part.size(),*fields[i]);
            if (parsed.ec!=std::errc{} || parsed.ptr!=part.data()+part.size()) return fail(ErrorCode::invalid_argument,"Invalid version number");
            if (i<2) input.remove_prefix(dot+1);
        }
        return result;
    }
};
} // namespace ii::core
