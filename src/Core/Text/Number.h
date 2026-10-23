#pragma once
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ii::core {
// The floating-point charconv overloads require macOS 26 in Apple's libc++.
// A classic-locale stream keeps the same decimal contract on supported systems.
inline std::optional<double> parse_decimal(std::string_view text) {
    if (text.empty() || text.size() > 1024) return {};
    std::size_t i = text.front() == '-' ? 1 : 0;
    bool digit = false, nonzero = false;
    const auto digits = [&] {
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            digit = true; nonzero = nonzero || text[i] != '0'; ++i;
        }
    };
    digits();
    if (i < text.size() && text[i] == '.') { ++i; digits(); }
    if (!digit) return {};
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
        const auto start = i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        if (i == start) return {};
    }
    if (i != text.size()) return {};
    if (!nonzero) return text.front()=='-'?-0.:0.;
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    double number = 0;
    input >> std::noskipws >> number;
    // libc++ sets failbit for a representable subnormal when strtod reports
    // ERANGE. Keep that finite, nonzero value; reject overflow and loss to zero.
    const bool subnormal = number != 0 && std::abs(number) < std::numeric_limits<double>::min();
    if (input.bad() || (input.fail() && !subnormal) || !input.eof() || !std::isfinite(number) || (number == 0 && nonzero)) return {};
    return number;
}
inline std::string format_decimal(double number) {
    if (!std::isfinite(number)) throw std::invalid_argument("A decimal must be finite");
    const auto render=[&](int precision,std::ios_base::fmtflags mode){std::ostringstream output;output.imbue(std::locale::classic());output.setf(mode,std::ios_base::floatfield);output<<std::setprecision(precision)<<number;return output.str();};
    int low=1,high=std::numeric_limits<double>::max_digits10;
    while(low<high){const auto middle=(low+high)/2;const auto value=parse_decimal(render(middle,std::ios_base::fmtflags{}));if(value&&*value==number)high=middle;else low=middle+1;}
    auto result=render(low,std::ios_base::fmtflags{});
    auto scientific=render(low-1,std::ios_base::scientific);if(scientific.size()<result.size())result=std::move(scientific);
    const int exponent=number==0?0:int(std::floor(std::log10(std::abs(number))));
    auto fixed=render(std::max(0,low-1-exponent),std::ios_base::fixed);
    if(fixed.size()<result.size()){const auto read=parse_decimal(fixed);if(read&&*read==number)result=std::move(fixed);}
    return result;
}
}
