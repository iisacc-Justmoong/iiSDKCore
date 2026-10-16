#pragma once
#include "Utf8.h"
#include <expected>
#include <filesystem>
#include <string>

namespace ii::core {
enum class TextPolicy { reject, replace };
enum class TextErrorCode { invalid_utf8, invalid_utf16, invalid_path };
struct TextError final { TextErrorCode code; std::size_t offset; };
template<class T> using TextResult=std::expected<T,TextError>;
namespace text_detail {
// Invalid input consumes one code unit/byte in replacement mode. No normalization.
inline void utf8(std::string &out, char32_t scalar) {
    if (scalar<0x80) out.push_back(char(scalar));
    else if (scalar<0x800) { out.push_back(char(0xc0|(scalar>>6))); out.push_back(char(0x80|(scalar&63))); }
    else if (scalar<0x10000) { out.push_back(char(0xe0|(scalar>>12))); out.push_back(char(0x80|((scalar>>6)&63))); out.push_back(char(0x80|(scalar&63))); }
    else { out.push_back(char(0xf0|(scalar>>18))); out.push_back(char(0x80|((scalar>>12)&63))); out.push_back(char(0x80|((scalar>>6)&63))); out.push_back(char(0x80|(scalar&63))); }
}
}
[[nodiscard]] inline TextResult<std::u16string> to_utf16(std::string_view input,TextPolicy policy=TextPolicy::reject) {
    std::u16string out;
    for (std::size_t index=0; index<input.size();) {
        const auto start=index;
        const auto first=static_cast<unsigned char>(input[index]);
        char32_t scalar=first;
        std::size_t count=1;
        char32_t minimum=0;
        if (first>=0xc2 && first<=0xdf) { scalar=first&31; count=2; minimum=0x80; }
        else if (first>=0xe0 && first<=0xef) { scalar=first&15; count=3; minimum=0x800; }
        else if (first>=0xf0 && first<=0xf4) { scalar=first&7; count=4; minimum=0x10000; }
        bool valid=first<0x80 || count>1;
        if (count>input.size()-index) valid=false;
        if (valid) for (std::size_t offset=1;offset<count;++offset) {
            const auto next=static_cast<unsigned char>(input[index+offset]);
            if ((next&0xc0)!=0x80) { valid=false; break; }
            scalar=(scalar<<6)|(next&63);
        }
        valid=valid && scalar>=minimum && scalar<=0x10ffff && !(scalar>=0xd800 && scalar<=0xdfff);
        if (!valid) {
            if (policy==TextPolicy::reject) return std::unexpected(TextError{TextErrorCode::invalid_utf8,start});
            scalar=0xfffd; count=1;
        }
        index+=count;
        if (scalar<0x10000) out.push_back(char16_t(scalar));
        else { scalar-=0x10000; out.push_back(char16_t(0xd800+(scalar>>10))); out.push_back(char16_t(0xdc00+(scalar&1023))); }
    }
    return out;
}
[[nodiscard]] inline TextResult<std::string> to_utf8(std::u16string_view input,TextPolicy policy=TextPolicy::reject) {
    std::string out;
    for (std::size_t index=0;index<input.size();++index) {
        char32_t scalar=input[index];
        if (scalar>=0xd800 && scalar<=0xdfff) {
            if (scalar<=0xdbff && index+1<input.size() && input[index+1]>=0xdc00 && input[index+1]<=0xdfff) {
                scalar=0x10000+((scalar-0xd800)<<10)+(input[++index]-0xdc00);
            } else {
                if (policy==TextPolicy::reject) return std::unexpected(TextError{TextErrorCode::invalid_utf16,index});
                scalar=0xfffd;
            }
        }
        text_detail::utf8(out,scalar);
    }
    return out;
}
// Explicit UTF-8/native conversion; native path operations otherwise retain native bytes.
[[nodiscard]] inline TextResult<std::filesystem::path> path_from_utf8(std::string_view text) {
    if (!valid_utf8(text) || text.find('\0')!=text.npos)
        return std::unexpected(TextError{TextErrorCode::invalid_path,0});
    try {
        std::u8string bytes;
        bytes.reserve(text.size());
        for (unsigned char byte:text) bytes.push_back(char8_t(byte));
        return std::filesystem::path(bytes);
    } catch (const std::filesystem::filesystem_error &) {
        return std::unexpected(TextError{TextErrorCode::invalid_path,0});
    }
}
[[nodiscard]] inline TextResult<std::string> path_to_utf8(const std::filesystem::path &path) {
    try {
        auto bytes=path.u8string();
        std::string text;
        text.reserve(bytes.size());
        for (char8_t byte:bytes) text.push_back(char(byte));
        if (!valid_utf8(text) || text.find('\0')!=text.npos)
            return std::unexpected(TextError{TextErrorCode::invalid_path,0});
        return text;
    } catch (const std::filesystem::filesystem_error &) {
        return std::unexpected(TextError{TextErrorCode::invalid_path,0});
    }
}
} // namespace ii::core
