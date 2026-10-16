#include <iiSDKCore/Core/Text/Text.h>
#include <iostream>
#include <stdexcept>
using namespace ii::core;
int main() {
    int checks=0;
    auto check=[&](bool value) { ++checks; if (!value) throw std::runtime_error("text check "+std::to_string(checks)); };
    for (std::string_view text : {"", "ASCII", "한글", "🎵", "e\xcc\x81"}) {
        const auto wide=to_utf16(text);
        check(wide.has_value()); check(to_utf8(*wide).value()==text);
    }
    check(to_utf16(std::string_view("a\0b",3))->size()==3);
    check(to_utf8(std::u16string_view(u"a\0b",3))->size()==3);
    check(to_utf16("🎵")->size()==2 && std::string_view("🎵").size()==4);
    for (std::string_view text : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80"}) {
        auto result=to_utf16(text); check(!result && result.error().code==TextErrorCode::invalid_utf8);
        check(valid_utf8(to_utf8(to_utf16(text,TextPolicy::replace).value()).value()));
    }
    check(!to_utf8(std::u16string(1,char16_t(0xd800))));
    check(!to_utf8(std::u16string(1,char16_t(0xdc00))));
    check(to_utf8(std::u16string(1,char16_t(0xd800)),TextPolicy::replace).value()=="\xef\xbf\xbd");
    check(!path_from_utf8(std::string_view("a\0b",3)));
    check(!path_from_utf8("\xff"));
    auto path=path_from_utf8("한글/🎵.mid"); check(path.has_value());
    check(path_to_utf8(*path).value()=="한글/🎵.mid");
#ifndef _WIN32
    check(!path_to_utf8(std::filesystem::path(std::string("\xff",1))));
#endif
    const auto error=to_utf16("a\xff"); check(error.error().offset==1);
    std::cout << checks << " text boundary checks passed\n";
}
