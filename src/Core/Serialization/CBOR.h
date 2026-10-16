#pragma once
#include "../Text/Utf8.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace ii::serialization {
using Bytes = core::Bytes;
enum class ErrorCode {
  truncated,
  invalid_format,
  invalid_utf8,
  type_mismatch,
  out_of_range,
  limit_exceeded,
  duplicate_key,
  unsupported,
  unknown_type,
  schema_mismatch,
  codec_failed
};
struct Error final {
  ErrorCode code;
  std::size_t offset;
  // Library messages have static storage; custom codecs must use static text.
  const char *message;
  std::exception_ptr cause{};
};
template <class T> using Result = std::expected<T, Error>;
struct Limits final {
  std::size_t max_bytes = 16 * 1024 * 1024;
  std::size_t max_blob_bytes = 8 * 1024 * 1024;
  std::size_t max_container_items = 100'000;
  std::size_t max_items = 200'000;
  std::size_t max_depth = 64;
  // Cumulative requested storage, excluding allocator metadata and custom
  // codecs.
  std::size_t max_allocation_bytes = 32 * 1024 * 1024;
};
class Encoder;
class Decoder;
template <class T> struct Codec;
template <class T>
concept Serializable = requires(Encoder &out, Decoder &in, const T &value) {
  { Codec<T>::encode(out, value) } -> std::same_as<Result<void>>;
  { Codec<T>::decode(in) } -> std::same_as<Result<T>>;
};
namespace detail {
inline bool utf8(std::span<const std::byte> bytes) { return core::valid_utf8(bytes); }
inline std::optional<std::uint16_t> half_exact(double value) {
  if (std::isnan(value))
    return 0x7e00;
  const std::uint16_t sign = std::signbit(value) ? 0x8000 : 0;
  if (std::isinf(value))
    return std::uint16_t(sign | 0x7c00);
  if (value == 0)
    return sign;
  if (std::abs(value) > std::numeric_limits<float>::max())
    return {};
  const float narrow = static_cast<float>(value);
  if (static_cast<double>(narrow) != value)
    return {};
  const auto bits = std::bit_cast<std::uint32_t>(narrow);
  const int exponent = int((bits >> 23) & 0xff) - 127;
  const auto fraction = bits & 0x7fffff;
  if (exponent >= -14 && exponent <= 15 && !(fraction & 0x1fff))
    return std::uint16_t(sign | ((exponent + 15) << 10) | (fraction >> 13));
  if (exponent >= -24 && exponent < -14) {
    const auto significand = 0x800000 | fraction;
    const auto shift = unsigned(-exponent - 1);
    if (!(significand & ((std::uint32_t{1} << shift) - 1)))
      return std::uint16_t(sign | (significand >> shift));
  }
  return {};
}
inline double half_value(std::uint16_t bits) {
  const auto exponent = (bits >> 10) & 31, fraction = bits & 1023;
  double value;
  if (exponent == 31)
    value = fraction ? std::numeric_limits<double>::quiet_NaN()
                     : std::numeric_limits<double>::infinity();
  else if (!exponent)
    value = std::ldexp(double(fraction), -24);
  else
    value = std::ldexp(double(1024 + fraction), int(exponent) - 25);
  return (bits & 0x8000) ? -value : value;
}
struct Depth final {
  std::size_t &value;
  explicit Depth(std::size_t &value) : value(value) { ++value; }
  ~Depth() { --value; }
};
} // namespace detail

// One independent encoder/decoder per message; no global registry or state.
class Encoder final {
public:
  explicit Encoder(Limits limits = {}) : limits_(limits) {}
  std::size_t position() const noexcept { return bytes_.size(); }
  const Limits &limits() const noexcept { return limits_; }
  template <class T> Result<T> reject(ErrorCode code, const char *message) {
    if (!error_)
      error_ = Error{code, position(), message, {}};
    return std::unexpected(*error_);
  }
  template <Serializable T> Result<void> write(const T &value);
  Result<void> null() { return simple(22); }
  Result<void> boolean(bool value) { return simple(value ? 21 : 20); }
  Result<void> unsigned_integer(std::uint64_t value) { return head(0, value); }
  // CBOR major type 1 represents -1-argument, including below INT64_MIN.
  Result<void> negative_integer(std::uint64_t argument) {
    return head(1, argument);
  }
  Result<void> array(std::size_t size) { return container(4, size); }
  Result<void> map(std::size_t size) { return container(5, size); }
  Result<void> text(std::string_view value) {
    if (value.size() > limits_.max_blob_bytes)
      return reject<void>(ErrorCode::limit_exceeded, "Encoded string limit");
    const auto bytes = std::as_bytes(std::span(value.data(), value.size()));
    if (!detail::utf8(bytes))
      return reject<void>(ErrorCode::invalid_utf8, "Text is not UTF-8");
    return blob(3, bytes);
  }
  Result<void> binary(std::span<const std::byte> value) {
    return blob(2, value);
  }
  Result<void> floating(double value) {
    static_assert(sizeof(double) == 8 && sizeof(float) == 4 &&
                  std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<float>::is_iec559);
    if (auto r = item(); !r)
      return r;
    if (auto half = detail::half_exact(value))
      return raw(0xf9, *half, 2);
    if (std::abs(value) <= std::numeric_limits<float>::max()) {
      const float narrow = static_cast<float>(value);
      if (double(narrow) == value)
        return raw(0xfa, std::bit_cast<std::uint32_t>(narrow), 4);
    }
    return raw(0xfb, std::bit_cast<std::uint64_t>(value), 8);
  }
  Result<void> allocate(std::size_t count, std::size_t width) {
    if (error_)
      return std::unexpected(*error_);
    if (width && count > (limits_.max_allocation_bytes - allocated_) / width)
      return reject<void>(ErrorCode::limit_exceeded,
                          "Encoder scratch storage limit");
    allocated_ += count * width;
    return {};
  }
  [[nodiscard]] Result<Bytes> finish();

private:
  Limits limits_;
  Bytes bytes_;
  std::optional<Error> error_;
  std::size_t items_ = 0, depth_ = 0, allocated_ = 0;
  Result<void> item() {
    if (error_)
      return std::unexpected(*error_);
    if (items_ >= limits_.max_items)
      return reject<void>(ErrorCode::limit_exceeded, "Encoded item limit");
    ++items_;
    return {};
  }
  Result<void> append(std::span<const std::byte> bytes) {
    if (error_)
      return std::unexpected(*error_);
    if (bytes.size() > limits_.max_bytes - bytes_.size())
      return reject<void>(ErrorCode::limit_exceeded, "Encoded byte limit");
    const auto required = bytes_.size() + bytes.size();
    if (required > bytes_.capacity()) {
      const auto grown = bytes_.capacity() > limits_.max_bytes / 2
                             ? limits_.max_bytes
                             : bytes_.capacity() * 2;
      bytes_.reserve(std::max(required, grown));
    }
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    return {};
  }
  Result<void> raw(unsigned initial, std::uint64_t argument, unsigned width) {
    std::array<std::byte, 9> bytes{};
    bytes[0] = std::byte(initial);
    for (unsigned i = 0; i < width; ++i)
      bytes[i + 1] = std::byte(argument >> ((width - i - 1) * 8));
    return append(std::span(bytes).first(width + 1));
  }
  Result<void> head(unsigned major, std::uint64_t argument) {
    if (auto r = item(); !r)
      return r;
    if (argument < 24)
      return raw((major << 5) | unsigned(argument), 0, 0);
    const unsigned width = argument <= 0xff         ? 1
                           : argument <= 0xffff     ? 2
                           : argument <= 0xffffffff ? 4
                                                    : 8;
    return raw((major << 5) | (width == 1   ? 24
                               : width == 2 ? 25
                               : width == 4 ? 26
                                            : 27),
               argument, width);
  }
  Result<void> simple(unsigned argument) { return head(7, argument); }
  Result<void> container(unsigned major, std::size_t size) {
    const auto multiplier = major == 5 ? 2u : 1u;
    if (size > limits_.max_container_items ||
        size > (limits_.max_items - items_) / multiplier)
      return reject<void>(ErrorCode::limit_exceeded, "Encoded container limit");
    return head(major, size);
  }
  Result<void> blob(unsigned major, std::span<const std::byte> value) {
    if (value.size() > limits_.max_blob_bytes)
      return reject<void>(ErrorCode::limit_exceeded, "Encoded string limit");
    if (auto r = head(major, value.size()); !r)
      return r;
    return append(value);
  }
};

class Decoder final {
public:
  explicit Decoder(std::span<const std::byte> bytes, Limits limits = {})
      : bytes_(bytes), limits_(limits) {
    if (bytes.size() > limits.max_bytes)
      error_ = Error{ErrorCode::limit_exceeded, 0, "Input byte limit", {}};
  }
  std::size_t position() const noexcept { return position_; }
  std::size_t remaining() const noexcept { return bytes_.size() - position_; }
  const Limits &limits() const noexcept { return limits_; }
  template <class T> Result<T> reject(ErrorCode code, const char *message) {
    if (!error_)
      error_ = Error{code, position_, message, {}};
    return std::unexpected(*error_);
  }
  template <Serializable T> Result<T> read();
  Result<void> null() {
    auto h = head();
    if (!h)
      return std::unexpected(h.error());
    if (h->major != 7 || h->additional != 22)
      return reject<void>(ErrorCode::type_mismatch, "Expected null");
    return {};
  }
  Result<bool> boolean() {
    auto h = head();
    if (!h)
      return std::unexpected(h.error());
    if (h->major != 7 || (h->additional != 20 && h->additional != 21))
      return reject<bool>(ErrorCode::type_mismatch, "Expected boolean");
    return h->additional == 21;
  }
  template <std::integral T>
    requires(!std::same_as<T, bool> && sizeof(T) <= 8)
  Result<T> integer() {
    auto h = head();
    if (!h)
      return std::unexpected(h.error());
    if (h->major > 1)
      return reject<T>(ErrorCode::type_mismatch, "Expected integer");
    const auto maximum = std::uint64_t(std::numeric_limits<T>::max());
    if (h->argument > maximum || (h->major == 1 && !std::is_signed_v<T>))
      return reject<T>(ErrorCode::out_of_range, "Integer outside target range");
    if constexpr (std::is_signed_v<T>) {
      if (h->major == 1)
        return T(-1 - static_cast<T>(h->argument));
    }
    return static_cast<T>(h->argument);
  }
  Result<double> floating() {
    auto h = head();
    if (!h)
      return std::unexpected(h.error());
    if (h->major != 7 || h->additional < 25)
      return reject<double>(ErrorCode::type_mismatch,
                            "Expected IEEE floating point");
    if (h->additional == 25)
      return detail::half_value(std::uint16_t(h->argument));
    if (h->additional == 26)
      return double(std::bit_cast<float>(std::uint32_t(h->argument)));
    return std::bit_cast<double>(h->argument);
  }
  Result<std::size_t> array() { return container(4); }
  Result<std::size_t> map() { return container(5); }
  Result<std::string> text() {
    auto data = blob(3);
    if (!data)
      return std::unexpected(data.error());
    if (auto r = allocate(data->size(), 1); !r)
      return std::unexpected(r.error());
    return std::string(reinterpret_cast<const char *>(data->data()),
                       data->size());
  }
  Result<Bytes> binary() {
    auto data = blob(2);
    if (!data)
      return std::unexpected(data.error());
    if (auto r = allocate(data->size(), 1); !r)
      return std::unexpected(r.error());
    return Bytes(data->begin(), data->end());
  }
  Result<void> allocate(std::size_t count, std::size_t width) {
    if (error_)
      return std::unexpected(*error_);
    if (width && count > (limits_.max_allocation_bytes - allocated_) / width)
      return reject<void>(ErrorCode::limit_exceeded, "Decoded storage limit");
    allocated_ += count * width;
    return {};
  }
  Result<void> finish() {
    if (error_)
      return std::unexpected(*error_);
    if (remaining())
      return reject<void>(ErrorCode::invalid_format,
                          "Trailing bytes after CBOR item");
    return {};
  }
  // Validates one profile item without constructing a typed payload.
  Result<void> skip(bool require_sorted_maps = false) {
    if (error_)
      return std::unexpected(*error_);
    if (depth_ >= limits_.max_depth)
      return reject<void>(ErrorCode::limit_exceeded, "CBOR nesting limit");
    detail::Depth depth(depth_);
    auto h = head();
    if (!h)
      return std::unexpected(h.error());
    if (h->major == 2 || h->major == 3) {
      auto data = blob_data(*h);
      if (!data)
        return std::unexpected(data.error());
    } else if (h->major == 4 || h->major == 5) {
      auto count = container_size(*h);
      if (!count)
        return std::unexpected(count.error());
      std::unordered_set<std::string> keys;
      std::optional<std::string_view> previous;
      if (h->major == 5) {
        if (auto r = allocate(*count, sizeof(std::string) + 3 * sizeof(void *));
            !r)
          return r;
      }
      for (std::size_t i = 0; i < *count; ++i) {
        if (h->major == 5) {
          if (depth_ >= limits_.max_depth)
            return reject<void>(ErrorCode::limit_exceeded,
                                "CBOR nesting limit");
          auto key = text();
          if (!key)
            return std::unexpected(key.error());
          auto [entry, inserted] = keys.insert(std::move(*key));
          if (!inserted)
            return reject<void>(ErrorCode::duplicate_key, "Duplicate map key");
          if (require_sorted_maps && previous &&
              (previous->size() > entry->size() ||
               (previous->size() == entry->size() && *previous >= *entry)))
            return reject<void>(
                ErrorCode::invalid_format,
                "Encoder map keys must be ordered by CBOR encoding");
          previous = *entry;
        }
        if (auto r = skip(require_sorted_maps); !r)
          return r;
      }
    }
    return {};
  }

private:
  struct Head final {
    unsigned major, additional;
    std::uint64_t argument;
  };
  std::span<const std::byte> bytes_;
  Limits limits_;
  std::optional<Error> error_;
  std::size_t position_ = 0, items_ = 0, depth_ = 0, allocated_ = 0;
  Result<Head> head() {
    if (error_)
      return std::unexpected(*error_);
    if (items_ >= limits_.max_items)
      return reject<Head>(ErrorCode::limit_exceeded, "Decoded item limit");
    if (!remaining())
      return reject<Head>(ErrorCode::truncated, "Missing CBOR header");
    ++items_;
    const unsigned initial = std::to_integer<unsigned>(bytes_[position_++]);
    Head h{initial >> 5, initial & 31, initial & 31};
    if (h.additional >= 28 && h.additional <= 30)
      return reject<Head>(ErrorCode::invalid_format,
                          "Reserved CBOR additional information");
    if (h.additional == 31 || h.major == 6)
      return reject<Head>(
          ErrorCode::unsupported,
          "CBOR tags and indefinite lengths are outside this profile");
    if (h.additional >= 24) {
      const unsigned width = 1u << (h.additional - 24);
      if (remaining() < width)
        return reject<Head>(ErrorCode::truncated, "Truncated CBOR argument");
      h.argument = 0;
      for (unsigned i = 0; i < width; ++i)
        h.argument =
            (h.argument << 8) | std::to_integer<unsigned>(bytes_[position_++]);
    }
    if (h.major == 7 &&
        !(h.additional == 20 || h.additional == 21 || h.additional == 22 ||
          (h.additional >= 25 && h.additional <= 27)))
      return reject<Head>(ErrorCode::unsupported,
                          "Unsupported CBOR simple value");
    return h;
  }
  Result<std::size_t> container_size(const Head &h) {
    const auto multiplier = h.major == 5 ? 2u : 1u;
    if (h.argument > limits_.max_container_items ||
        h.argument > (limits_.max_items - items_) / multiplier)
      return reject<std::size_t>(ErrorCode::limit_exceeded,
                                 "Decoded container limit");
    if (h.argument > remaining() / multiplier)
      return reject<std::size_t>(ErrorCode::truncated,
                                 "Container exceeds remaining input");
    return std::size_t(h.argument);
  }
  Result<std::size_t> container(unsigned major) {
    auto h = head();
    if (!h)
      return std::unexpected(h.error());
    if (h->major != major)
      return reject<std::size_t>(ErrorCode::type_mismatch,
                                 "Expected CBOR container");
    return container_size(*h);
  }
  Result<std::span<const std::byte>> blob_data(const Head &h) {
    if (h.argument > limits_.max_blob_bytes)
      return reject<std::span<const std::byte>>(ErrorCode::limit_exceeded,
                                                "Decoded string limit");
    if (h.argument > remaining())
      return reject<std::span<const std::byte>>(
          ErrorCode::truncated, "String exceeds remaining input");
    auto result = bytes_.subspan(position_, std::size_t(h.argument));
    if (h.major == 3 && !detail::utf8(result))
      return reject<std::span<const std::byte>>(ErrorCode::invalid_utf8,
                                                "Text is not UTF-8");
    position_ += result.size();
    return result;
  }
  Result<std::span<const std::byte>> blob(unsigned major) {
    auto h = head();
    if (!h)
      return std::unexpected(h.error());
    if (h->major != major)
      return reject<std::span<const std::byte>>(ErrorCode::type_mismatch,
                                                "Expected CBOR string");
    return blob_data(*h);
  }
};

template <Serializable T> Result<void> Encoder::write(const T &value) {
  if (error_)
    return std::unexpected(*error_);
  if (depth_ >= limits_.max_depth)
    return reject<void>(ErrorCode::limit_exceeded, "Codec nesting limit");
  detail::Depth depth(depth_);
  try {
    auto result = Codec<T>::encode(*this, value);
    if (!result && !error_)
      error_ = result.error();
    return error_ ? Result<void>{std::unexpected(*error_)} : result;
  } catch (...) {
    if (!error_)
      error_ = Error{ErrorCode::codec_failed, position(), "Encoder codec threw",
                     std::current_exception()};
    return std::unexpected(*error_);
  }
}
template <Serializable T> Result<T> Decoder::read() {
  if (error_)
    return std::unexpected(*error_);
  if (depth_ >= limits_.max_depth)
    return reject<T>(ErrorCode::limit_exceeded, "Codec nesting limit");
  detail::Depth depth(depth_);
  try {
    auto result = Codec<T>::decode(*this);
    if (!result && !error_)
      error_ = result.error();
    if (error_)
      return std::unexpected(*error_);
    return result;
  } catch (...) {
    if (!error_)
      error_ = Error{ErrorCode::codec_failed, position_, "Decoder codec threw",
                     std::current_exception()};
    return std::unexpected(*error_);
  }
}
inline Result<Bytes> Encoder::finish() {
  if (error_)
    return std::unexpected(*error_);
  try {
    Decoder validator(bytes_, limits_);
    if (auto r = validator.skip(true); !r) {
      error_ = r.error();
      return std::unexpected(*error_);
    }
    if (auto r = validator.finish(); !r) {
      error_ = r.error();
      return std::unexpected(*error_);
    }
    return std::move(bytes_);
  } catch (...) {
    error_ = Error{ErrorCode::codec_failed, position(),
                   "Encoder validation threw", std::current_exception()};
    return std::unexpected(*error_);
  }
}
[[nodiscard]] inline Result<void> validate(std::span<const std::byte> bytes,
                                           Limits limits = {}) {
  Decoder validator(bytes, limits);
  try {
    if (auto r = validator.skip(); !r)
      return r;
    return validator.finish();
  } catch (...) {
    return std::unexpected(Error{ErrorCode::codec_failed, validator.position(),
                                 "CBOR validation threw",
                                 std::current_exception()});
  }
}
} // namespace ii::serialization
