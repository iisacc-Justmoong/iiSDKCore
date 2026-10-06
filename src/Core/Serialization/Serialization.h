#pragma once
#include "CBOR.h"

namespace ii::serialization {
template <std::integral T>
  requires(!std::same_as<T, bool> && sizeof(T) <= 8)
struct Codec<T> final {
  static Result<void> encode(Encoder &out, T value) {
    if constexpr (std::is_signed_v<T>) {
      if (value < 0)
        return out.negative_integer(std::uint64_t(-(value + 1)));
    }
    return out.unsigned_integer(std::uint64_t(value));
  }
  static Result<T> decode(Decoder &in) { return in.integer<T>(); }
};
template <> struct Codec<bool> final {
  static Result<void> encode(Encoder &out, bool value) {
    return out.boolean(value);
  }
  static Result<bool> decode(Decoder &in) { return in.boolean(); }
};
template <> struct Codec<std::nullptr_t> final {
  static Result<void> encode(Encoder &out, std::nullptr_t) {
    return out.null();
  }
  static Result<std::nullptr_t> decode(Decoder &in) {
    if (auto r = in.null(); !r)
      return std::unexpected(r.error());
    return nullptr;
  }
};
template <> struct Codec<std::monostate> final {
  static Result<void> encode(Encoder &out, std::monostate) {
    return out.null();
  }
  static Result<std::monostate> decode(Decoder &in) {
    if (auto result = in.null(); !result)
      return std::unexpected(result.error());
    return std::monostate{};
  }
};
template <class T>
  requires(std::same_as<T, float> || std::same_as<T, double>)
struct Codec<T> final {
  static Result<void> encode(Encoder &out, T value) {
    return out.floating(value);
  }
  static Result<T> decode(Decoder &in) {
    auto value = in.floating();
    if (!value)
      return std::unexpected(value.error());
    if constexpr (std::same_as<T, float>) {
      if (std::isfinite(*value) &&
          (std::abs(*value) > std::numeric_limits<float>::max() ||
           double(static_cast<float>(*value)) != *value))
        return in.reject<T>(ErrorCode::out_of_range,
                            "Floating point conversion would lose precision");
    }
    return static_cast<T>(*value);
  }
};
template <> struct Codec<std::string> final {
  static Result<void> encode(Encoder &out, const std::string &value) {
    return out.text(value);
  }
  static Result<std::string> decode(Decoder &in) { return in.text(); }
};
template <> struct Codec<Bytes> final {
  static Result<void> encode(Encoder &out, const Bytes &value) {
    return out.binary(value);
  }
  static Result<Bytes> decode(Decoder &in) { return in.binary(); }
};
template <Serializable T> struct Codec<std::vector<T>> final {
  static Result<void> encode(Encoder &out, const std::vector<T> &value) {
    if (auto r = out.array(value.size()); !r)
      return r;
    for (const auto &item : value)
      if (auto r = out.write<T>(item); !r)
        return r;
    return {};
  }
  static Result<std::vector<T>> decode(Decoder &in) {
    auto size = in.array();
    if (!size)
      return std::unexpected(size.error());
    if (auto r = in.allocate(*size, sizeof(T)); !r)
      return std::unexpected(r.error());
    std::vector<T> result;
    result.reserve(*size);
    for (std::size_t i = 0; i < *size; ++i) {
      auto item = in.read<T>();
      if (!item)
        return std::unexpected(item.error());
      result.push_back(std::move(*item));
    }
    return result;
  }
};
template <Serializable T, std::size_t N> struct Codec<std::array<T, N>> final {
  static Result<void> encode(Encoder &out, const std::array<T, N> &value) {
    if (auto r = out.array(N); !r)
      return r;
    for (const auto &item : value)
      if (auto r = out.write(item); !r)
        return r;
    return {};
  }
  static Result<std::array<T, N>> decode(Decoder &in) {
    auto size = in.array();
    if (!size)
      return std::unexpected(size.error());
    if (*size != N)
      return in.reject<std::array<T, N>>(ErrorCode::schema_mismatch,
                                         "Fixed array size mismatch");
    return elements(in, std::make_index_sequence<N>{});
  }

private:
  template <std::size_t... I>
  static Result<std::array<T, N>> elements(Decoder &in,
                                           std::index_sequence<I...>) {
    auto values = std::tuple{(static_cast<void>(I), in.read<T>())...};
    std::optional<Error> failure;
    std::apply(
        [&](const auto &...v) {
          ((v ? void() : void(failure.emplace(v.error()))), ...);
        },
        values);
    if (failure)
      return std::unexpected(*failure);
    return std::array<T, N>{std::move(*std::get<I>(values))...};
  }
};
template <std::size_t N> struct Codec<std::array<std::byte, N>> final {
  static Result<void> encode(Encoder &out,
                             const std::array<std::byte, N> &value) {
    return out.binary(value);
  }
  static Result<std::array<std::byte, N>> decode(Decoder &in) {
    auto bytes = in.binary();
    if (!bytes)
      return std::unexpected(bytes.error());
    if (bytes->size() != N)
      return in.reject<std::array<std::byte, N>>(
          ErrorCode::schema_mismatch, "Fixed byte array size mismatch");
    std::array<std::byte, N> result;
    std::copy(bytes->begin(), bytes->end(), result.begin());
    return result;
  }
};
template <Serializable T> struct Codec<std::optional<T>> final {
  static Result<void> encode(Encoder &out, const std::optional<T> &value) {
    if (auto r = out.array(value ? 1 : 0); !r)
      return r;
    return value ? out.write(*value) : Result<void>{};
  }
  static Result<std::optional<T>> decode(Decoder &in) {
    auto size = in.array();
    if (!size)
      return std::unexpected(size.error());
    if (*size > 1)
      return in.reject<std::optional<T>>(
          ErrorCode::schema_mismatch, "Optional must be zero or one element");
    if (!*size)
      return std::optional<T>{};
    auto value = in.read<T>();
    if (!value)
      return std::unexpected(value.error());
    return std::optional<T>{std::move(*value)};
  }
};
namespace detail {
template <class Map> Result<void> write_map(Encoder &out, const Map &value) {
  if (auto r = out.map(value.size()); !r)
    return r;
  using Pair = typename Map::value_type;
  if (auto r = out.allocate(value.size(), sizeof(const Pair *)); !r)
    return r;
  std::vector<const Pair *> sorted;
  sorted.reserve(value.size());
  for (const auto &pair : value)
    sorted.push_back(&pair);
  std::sort(sorted.begin(), sorted.end(), [](const Pair *a, const Pair *b) {
    if (a->first.size() != b->first.size())
      return a->first.size() < b->first.size();
    return a->first < b->first;
  });
  for (const auto *pair : sorted) {
    if (auto r = out.write(pair->first); !r)
      return r;
    if (auto r = out.write(pair->second); !r)
      return r;
  }
  return {};
}
template <class Map> Result<Map> read_map(Decoder &in) {
  auto size = in.map();
  if (!size)
    return std::unexpected(size.error());
  if (auto r = in.allocate(*size, sizeof(typename Map::value_type) +
                                      3 * sizeof(void *));
      !r)
    return std::unexpected(r.error());
  Map result;
  for (std::size_t i = 0; i < *size; ++i) {
    auto key = in.read<std::string>();
    if (!key)
      return std::unexpected(key.error());
    auto value = in.read<typename Map::mapped_type>();
    if (!value)
      return std::unexpected(value.error());
    if (!result.emplace(std::move(*key), std::move(*value)).second)
      return in.reject<Map>(ErrorCode::duplicate_key, "Duplicate map key");
  }
  return result;
}
} // namespace detail
template <Serializable T, class Compare, class Allocator>
struct Codec<std::map<std::string, T, Compare, Allocator>> final {
  using Map = std::map<std::string, T, Compare, Allocator>;
  static Result<void> encode(Encoder &out, const Map &value) {
    return detail::write_map(out, value);
  }
  static Result<Map> decode(Decoder &in) { return detail::read_map<Map>(in); }
};
template <Serializable T, class Hash, class Equal, class Allocator>
struct Codec<std::unordered_map<std::string, T, Hash, Equal, Allocator>> final {
  using Map = std::unordered_map<std::string, T, Hash, Equal, Allocator>;
  static Result<void> encode(Encoder &out, const Map &value) {
    return detail::write_map(out, value);
  }
  static Result<Map> decode(Decoder &in) { return detail::read_map<Map>(in); }
};
template <Serializable... T> struct Codec<std::tuple<T...>> final {
  static Result<void> encode(Encoder &out, const std::tuple<T...> &value) {
    if (auto r = out.array(sizeof...(T)); !r)
      return r;
    Result<void> result;
    std::apply(
        [&](const auto &...item) {
          ((result ? void(result = out.write(item)) : void()), ...);
        },
        value);
    return result;
  }
  static Result<std::tuple<T...>> decode(Decoder &in) {
    auto size = in.array();
    if (!size)
      return std::unexpected(size.error());
    if (*size != sizeof...(T))
      return in.reject<std::tuple<T...>>(ErrorCode::schema_mismatch,
                                         "Tuple size mismatch");
    return elements(in, std::index_sequence_for<T...>{});
  }

private:
  template <std::size_t... I>
  static Result<std::tuple<T...>> elements(Decoder &in,
                                           std::index_sequence<I...>) {
    // List initialization sequences reads; failed readers remain failed.
    auto values = std::tuple{in.read<T>()...};
    std::optional<Error> failure;
    std::apply(
        [&](const auto &...v) {
          ((v ? void() : void(failure.emplace(v.error()))), ...);
        },
        values);
    if (failure)
      return std::unexpected(*failure);
    return std::tuple<T...>{std::move(*std::get<I>(values))...};
  }
};
template <Serializable A, Serializable B> struct Codec<std::pair<A, B>> final {
  static Result<void> encode(Encoder &out, const std::pair<A, B> &value) {
    if (auto r = out.array(2); !r)
      return r;
    if (auto r = out.write(value.first); !r)
      return r;
    return out.write(value.second);
  }
  static Result<std::pair<A, B>> decode(Decoder &in) {
    auto size = in.array();
    if (!size)
      return std::unexpected(size.error());
    if (*size != 2)
      return in.reject<std::pair<A, B>>(ErrorCode::schema_mismatch,
                                        "Pair size mismatch");
    auto first = in.read<A>();
    if (!first)
      return std::unexpected(first.error());
    auto second = in.read<B>();
    if (!second)
      return std::unexpected(second.error());
    return std::pair<A, B>{std::move(*first), std::move(*second)};
  }
};
template <Serializable... T> struct Codec<std::variant<T...>> final {
  using Variant = std::variant<T...>;
  static Result<void> encode(Encoder &out, const Variant &value) {
    if (value.valueless_by_exception())
      return out.reject<void>(ErrorCode::codec_failed, "Variant has no value");
    if (auto r = out.array(2); !r)
      return r;
    if (auto r = out.write(std::uint64_t(value.index())); !r)
      return r;
    return std::visit([&](const auto &item) { return out.write(item); }, value);
  }
  static Result<Variant> decode(Decoder &in) {
    auto size = in.array();
    if (!size)
      return std::unexpected(size.error());
    if (*size != 2)
      return in.reject<Variant>(ErrorCode::schema_mismatch,
                                "Variant size mismatch");
    auto index = in.read<std::uint64_t>();
    if (!index)
      return std::unexpected(index.error());
    return alternative<0>(in, *index);
  }

private:
  template <std::size_t I>
  static Result<Variant> alternative(Decoder &in, std::uint64_t index) {
    if constexpr (I == sizeof...(T))
      return in.reject<Variant>(ErrorCode::schema_mismatch,
                                "Unknown variant alternative");
    else {
      if (index != I)
        return alternative<I + 1>(in, index);
      auto value = in.read<std::variant_alternative_t<I, Variant>>();
      if (!value)
        return std::unexpected(value.error());
      return Variant{std::in_place_index<I>, std::move(*value)};
    }
  }
};

template <Serializable T>
[[nodiscard]] Result<Bytes> serialize(const T &value, Limits limits = {}) {
  Encoder encoder(limits);
  if (auto r = encoder.write(value); !r)
    return std::unexpected(r.error());
  return encoder.finish();
}
template <Serializable T>
[[nodiscard]] Result<T> deserialize(std::span<const std::byte> bytes,
                                    Limits limits = {}) {
  if (auto r = validate(bytes, limits); !r)
    return std::unexpected(r.error());
  Decoder decoder(bytes, limits);
  auto result = decoder.read<T>();
  if (!result)
    return result;
  if (auto r = decoder.finish(); !r)
    return std::unexpected(r.error());
  return result;
}
} // namespace ii::serialization
