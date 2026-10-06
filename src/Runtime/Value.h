#pragma once

#include <any>
#include <concepts>
#include <expected>
#include <functional>
#include <typeindex>

namespace ii::runtime {

// Owning, copyable dynamic storage; exact C++ types and explicit borrowed
// reference_wrapper values retain the standard any lifetime contract.
using DynamicValue = std::any;
enum class CastErrorCode { empty, type_mismatch, unsupported, invalid_text, out_of_range, precision_loss };
struct CastError final {
    CastErrorCode code;
    std::type_index source{typeid(void)};
    std::type_index target{typeid(void)};
};
using CastResult = std::expected<DynamicValue, CastError>;

// Conversion is explicit and lossless for numeric-to-numeric casts.
// Allocation and user-defined copy exceptions propagate to the caller.
[[nodiscard]] CastResult cast_value(const DynamicValue& value, std::type_index target);

template<class T> requires std::copy_constructible<T> && (!std::is_reference_v<T>) && (!std::is_const_v<T>)
[[nodiscard]] std::expected<T, CastError> value_cast(const DynamicValue& value) {
    auto converted = cast_value(value, typeid(T));
    if (!converted) return std::unexpected(converted.error());
    return std::any_cast<T>(std::move(*converted));
}

// Borrowing does not copy or convert. Reassignment/reset/destruction invalidates
// the reference. Temporaries are rejected to prevent immediately dangling views.
template<class T> requires (!std::is_reference_v<T>)
[[nodiscard]] std::expected<std::reference_wrapper<const T>, CastError> value_ref(const DynamicValue& value) {
    if (const auto* found = std::any_cast<T>(&value)) return std::cref(*found);
    return std::unexpected(CastError{value.has_value() ? CastErrorCode::type_mismatch : CastErrorCode::empty,
                                     value.type(), typeid(T)});
}
template<class T> auto value_ref(DynamicValue&&) = delete;
template<class T> auto value_ref(const DynamicValue&&) = delete;

} // namespace ii::runtime
