#include <iiSDKCore/Core/Text/Number.h>
#include <cstdlib>
#include <iostream>
#include <clocale>
#include <bit>
#define CHECK(condition) do { if (!(condition)) { std::cerr << __LINE__ << ": " #condition "\n"; std::exit(1); } } while(false)
int main() {
    using namespace ii::core;
    for (const auto text : {"0", "-0", "0.1", "-1.25", "1e+20", "4.9406564584124654e-324", "1.7976931348623157e308"}) {
        const auto value = parse_decimal(text); CHECK(value);
        const auto again = parse_decimal(format_decimal(*value)); CHECK(again && *again == *value);
    }
    for (const auto text : {"", "+1", " 1", "1 ", "1x", "1,5", "0x1", "NaN", "inf", "1e", "--2", ".", "1e999", "1e-999"}) CHECK(!parse_decimal(text));
    // The process locale must never change serialized or edited decimal values.
    std::setlocale(LC_NUMERIC, "de_DE.UTF-8");
    CHECK(parse_decimal("1.5") == 1.5); CHECK(!parse_decimal("1,5")); CHECK(format_decimal(1.5) == "1.5");
    bool rejected = false;
    try { (void)format_decimal(std::numeric_limits<double>::infinity()); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    CHECK(format_decimal(.1)=="0.1");CHECK(format_decimal(1000)=="1000");
    std::uint64_t state=0x13198a2e03707344ULL;
    for(unsigned i=0;i<4000;++i){state^=state<<13;state^=state>>7;state^=state<<17;const auto value=std::bit_cast<double>(state);if(!std::isfinite(value))continue;const auto roundtrip=parse_decimal(format_decimal(value));CHECK(roundtrip&&*roundtrip==value);}
}
