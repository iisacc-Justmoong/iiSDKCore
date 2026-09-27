#include "Value.h"

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace ii::runtime {
namespace {
template<class T> using Converted = std::expected<T, CastErrorCode>;
auto fail(CastErrorCode code) { return std::unexpected(code); }

template<class T, class S> Converted<T> numeric(S value) {
    if constexpr (std::is_same_v<T, S>) return value;
    else if constexpr (std::is_same_v<T, bool>) {
        if (value != S(0) && value != S(1)) return fail(CastErrorCode::out_of_range);
        return value == S(1);
    } else if constexpr (std::is_integral_v<S> && std::is_integral_v<T>) {
        if constexpr (std::is_signed_v<S>) {
            const auto wide = static_cast<std::intmax_t>(value);
            if constexpr (std::is_signed_v<T>) {
                if (wide < std::numeric_limits<T>::min() || wide > std::numeric_limits<T>::max())
                    return fail(CastErrorCode::out_of_range);
            } else if (wide < 0 || static_cast<std::uintmax_t>(wide) > std::numeric_limits<T>::max())
                return fail(CastErrorCode::out_of_range);
        } else if (static_cast<std::uintmax_t>(value) > static_cast<std::uintmax_t>(std::numeric_limits<T>::max()))
            return fail(CastErrorCode::out_of_range);
        return static_cast<T>(value);
    } else if constexpr (std::is_floating_point_v<S> && std::is_integral_v<T>) {
        if (!std::isfinite(value)) return fail(CastErrorCode::out_of_range);
        // A power-of-two exclusive upper bound avoids rounded integer maxima,
        // including uint64 -> double rounding to 2^64 on Windows/WASM.
        const auto upper = std::ldexp(S(1), std::numeric_limits<T>::digits);
        const auto lower = std::is_signed_v<T> ? -upper : S(0);
        if (value < lower || value >= upper) return fail(CastErrorCode::out_of_range);
        if (std::trunc(value) != value) return fail(CastErrorCode::precision_loss);
        return static_cast<T>(value);
    } else if constexpr (std::is_integral_v<S> && std::is_floating_point_v<T>) {
        std::uintmax_t magnitude = static_cast<std::uintmax_t>(value);
        if constexpr (std::is_signed_v<S>) if (value < 0) magnitude = std::uintmax_t(0) - magnitude;
        if (magnitude && static_cast<int>(std::bit_width(magnitude)) - std::countr_zero(magnitude) > std::numeric_limits<T>::digits)
            return fail(CastErrorCode::precision_loss);
        return static_cast<T>(value);
    } else {
        if (!std::isfinite(value)) return fail(CastErrorCode::out_of_range);
        if constexpr (std::numeric_limits<T>::max_exponent <= std::numeric_limits<S>::max_exponent) {
            if (value > static_cast<S>(std::numeric_limits<T>::max()) || value < -static_cast<S>(std::numeric_limits<T>::max()))
                return fail(CastErrorCode::out_of_range);
        }
        const auto result = static_cast<T>(value);
        if (static_cast<S>(result) != value) return fail(CastErrorCode::precision_loss);
        return result;
    }
}

bool decimal(std::string_view text, bool& nonzero) {
    std::size_t i = 0, digits = 0;
    if (i < text.size() && text[i] == '-') ++i;
    const auto scan = [&] {
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            nonzero |= text[i] != '0';
            ++digits;
            ++i;
        }
    };
    scan();
    if (i < text.size() && text[i] == '.') { ++i; scan(); }
    if (!digits) return false;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '-' || text[i] == '+')) ++i;
        const auto start = i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        if (i == start) return false;
    }
    return i == text.size();
}

template<class T> Converted<T> parse(const std::string& text) {
    if constexpr (std::is_same_v<T, bool>) {
        if (text == "true" || text == "1") return true;
        if (text == "false" || text == "0") return false;
        return fail(CastErrorCode::invalid_text);
    } else if constexpr (std::is_same_v<T, long double>) {
        // Some supported WASM libraries format long double through double.
        // Until a portable full-precision codec exists, reject this pair on
        // every platform rather than quietly lose bits on one platform.
        return fail(CastErrorCode::unsupported);
    } else if constexpr (std::is_floating_point_v<T>) {
        bool nonzero = false;
        if (!decimal(text, nonzero)) return fail(CastErrorCode::invalid_text);
        if (!nonzero) return text.front() == '-' ? -T(0) : T(0);
        // Floating charconv is absent in some WASM libraries and older Apple
        // deployment targets. A per-stream classic locale works on all targets
        // without changing process-global locale or narrowing via double.
        std::istringstream input(text);
        input.imbue(std::locale::classic());
        T value{};
        input >> std::noskipws >> value;
        // Some libraries report ERANGE for representable subnormals. Accept
        // those results, but never a saturated maximum or underflow to zero.
        if (!input.eof() || !std::isfinite(value) || value == T(0) ||
            (input.fail() && std::fpclassify(value) != FP_SUBNORMAL))
            return fail(CastErrorCode::out_of_range);
        return value;
    } else {
        T value{};
        auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec == std::errc::result_out_of_range) return fail(CastErrorCode::out_of_range);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return fail(CastErrorCode::invalid_text);
        return value;
    }
}
template<class S> Converted<std::string> format(S value) {
    if constexpr (std::is_same_v<S, bool>) return std::string(value ? "true" : "false");
    else if constexpr (std::is_same_v<S, long double>) return fail(CastErrorCode::unsupported);
    else if constexpr (std::is_floating_point_v<S>) {
        if (!std::isfinite(value)) return fail(CastErrorCode::out_of_range);
        std::ostringstream output;
        output.imbue(std::locale::classic());
        output << std::setprecision(std::numeric_limits<S>::max_digits10) << value;
        return output.str();
    }
    else {
        std::array<char, 128> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (result.ec != std::errc{}) return fail(CastErrorCode::out_of_range);
        return std::string(buffer.data(), result.ptr);
    }
}
template<class T, class S> Converted<T> convert(S value) {
    if constexpr (std::is_same_v<T, std::string>) return format(value);
    else return numeric<T>(value);
}

// One list drives both source dispatch and runtime-selected destination types.
#define II_VALUE_NUMBERS(M) \
    M(bool) M(char) M(signed char) M(unsigned char) M(short) M(unsigned short) \
    M(int) M(unsigned int) M(long) M(unsigned long) M(long long) M(unsigned long long) \
    M(float) M(double) M(long double)

template<class T> Converted<T> convert_from(const DynamicValue& source) {
    if constexpr (!std::is_same_v<T, std::string>)
        if (const auto* text = std::any_cast<std::string>(&source)) return parse<T>(*text);
#define II_SOURCE(S) if (const auto* value = std::any_cast<S>(&source)) return convert<T>(*value);
    II_VALUE_NUMBERS(II_SOURCE)
#undef II_SOURCE
    return fail(CastErrorCode::unsupported);
}
template<class T> CastResult target_cast(const DynamicValue& source) {
    auto result = convert_from<T>(source);
    if (!result) return std::unexpected(CastError{result.error(), source.type(), typeid(T)});
    return DynamicValue(std::move(*result));
}
}

CastResult cast_value(const DynamicValue& value, std::type_index target) {
    if (!value.has_value()) return std::unexpected(CastError{CastErrorCode::empty, value.type(), target});
    if (value.type() == target) return value;
#define II_TARGET(T) if (target == typeid(T)) return target_cast<T>(value);
    II_VALUE_NUMBERS(II_TARGET)
#undef II_TARGET
#undef II_VALUE_NUMBERS
    if (target == typeid(std::string)) return target_cast<std::string>(value);
    return std::unexpected(CastError{CastErrorCode::unsupported, value.type(), target});
}
} // namespace ii::runtime
