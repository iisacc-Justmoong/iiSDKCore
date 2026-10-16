#pragma once
#include "../Result/Result.h"
#include "../Text/Utf8.h"
#include <cmath>
#include <concepts>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <variant>
#include <type_traits>
#include <utility>

namespace ii::core {
enum class ValueKind { null, boolean, signed_integer, unsigned_integer, real, text, bytes, array, object };
// Owned structured data, independent of JSON parsing/serialization and Runtime.
// Exact extraction preserves signed/unsigned/real distinctions; no truthiness,
// string-to-number or lossy numeric conversions are implicit. Read-only views
// prevent mutation from bypassing UTF-8/finite-number invariants.
class Value final {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;
    using Storage = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double,
        std::string, Bytes, Array, Object>;
    Value() = default;
    explicit Value(std::nullptr_t) {}
    template<class T> requires std::same_as<T, bool>
    explicit Value(T value) : data_(value) {}
    template<std::integral T> requires(!std::same_as<T, bool> && sizeof(T) <= 8)
    explicit Value(T value) {
        if constexpr (std::is_signed_v<T>) data_ = static_cast<std::int64_t>(value);
        else data_ = static_cast<std::uint64_t>(value);
    }
    template<std::floating_point T> requires(!std::same_as<T, long double>)
    explicit Value(T value) : data_(static_cast<double>(value)) {
        if (!std::isfinite(value)) throw std::invalid_argument("Value requires a finite number");
    }
    explicit Value(std::string value) : data_(std::move(value)) {
        if (!valid_utf8(std::get<std::string>(data_))) throw std::invalid_argument("Value text requires UTF-8");
    }
    explicit Value(std::string_view value) : Value(std::string(value)) {}
    explicit Value(const char *value) : Value(value ? std::string(value) : throw std::invalid_argument("Null text pointer")) {}
    explicit Value(Bytes value) : data_(std::move(value)) {}
    explicit Value(Array value) : data_(std::move(value)) {}
    explicit Value(Object value) : data_(std::move(value)) {
        for (const auto &[key, item] : std::get<Object>(data_)) {
            (void)item;
            if (!valid_utf8(key)) throw std::invalid_argument("Value object keys require UTF-8");
        }
    }
    [[nodiscard]] ValueKind kind() const noexcept { return static_cast<ValueKind>(data_.index()); }
    [[nodiscard]] const Storage &storage() const & noexcept { return data_; }
    const Storage &storage() const && = delete;
    template<class T> [[nodiscard]] Result<std::reference_wrapper<const T>> get() const & {
        if (const auto *value = std::get_if<T>(&data_)) return std::cref(*value);
        return fail(ErrorCode::invalid_argument, "Value type does not match the requested exact type");
    }
    template<class T> Result<std::reference_wrapper<const T>> get() const && = delete;
    [[nodiscard]] Result<std::reference_wrapper<const Value>> find(std::string_view key) const & {
        const auto object = get<Object>();
        if (!object) return std::unexpected(object.error());
        const auto found = object->get().find(key);
        if (found == object->get().end()) return fail(ErrorCode::not_found, "Value object key was not found");
        return std::cref(found->second);
    }
    Result<std::reference_wrapper<const Value>> find(std::string_view) const && = delete;
    [[nodiscard]] Result<std::reference_wrapper<const Value>> at(std::size_t index) const & {
        const auto array = get<Array>();
        if (!array) return std::unexpected(array.error());
        if (index >= array->get().size()) return fail(ErrorCode::out_of_range, "Value array index is out of range");
        return std::cref(array->get()[index]);
    }
    Result<std::reference_wrapper<const Value>> at(std::size_t) const && = delete;
    bool operator==(const Value &) const = default;
private:
    Storage data_;
};
} // namespace ii::core
