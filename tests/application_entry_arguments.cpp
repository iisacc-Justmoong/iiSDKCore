#include "../src/Runtime/ApplicationEntryArguments.h"
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    wchar_t executable[] = L"program.exe";
    wchar_t korean[] = {0xD55C, 0xAE00, 0};
    wchar_t emoji[] = {0xD83C, 0xDFA8, 0};
    wchar_t empty[] = {0};
    std::array<wchar_t*, 6> inputs{executable, korean, emoji, empty, korean, empty};
    auto result = ii::runtime::detail::decode_windows_arguments(inputs);
    check(result && *result == std::vector<std::string>({"program.exe", "\xed\x95\x9c\xea\xb8\x80", "\xf0\x9f\x8e\xa8", "", "\xed\x95\x9c\xea\xb8\x80", ""}),
          "Windows arguments lost Unicode, ordering, duplicates, or empty strings");
    std::array<wchar_t*, 1> null_input{nullptr};
    check(!ii::runtime::detail::decode_windows_arguments(null_input), "Null argument was accepted");
    wchar_t incomplete[] = {0xD83C, 0};
    std::array<wchar_t*, 1> invalid{incomplete};
    check(!ii::runtime::detail::decode_windows_arguments(invalid), "Unpaired surrogate was accepted");
    if constexpr (sizeof(wchar_t) > 2) {
        wchar_t non_utf16[] = {static_cast<wchar_t>(0x110000), 0};
        std::array<wchar_t*, 1> non_utf16_input{non_utf16};
        check(!ii::runtime::detail::decode_windows_arguments(non_utf16_input), "Invalid UTF-16 unit was accepted");
    }
}
