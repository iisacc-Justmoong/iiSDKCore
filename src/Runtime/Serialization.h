#pragma once
#include "../Core/Serialization/Serialization.h"
#include "Value.h"
#include <mutex>
#include <shared_mutex>
#include <stdexcept>

namespace ii::runtime {
// Explicit C++ type -> stable wire ID mapping. No typeid name/hash on the wire.
// Entries contain stateless function pointers; codecs run outside the lock.
class DynamicSerializer final {
public:
  explicit DynamicSerializer(bool builtins = true, std::size_t capacity = 64)
      : capacity_(capacity) {
    if (!builtins)
      return;
    builtin<bool>("cpp.bool");
    builtin<char>("cpp.char");
    builtin<signed char>("cpp.signed-char");
    builtin<unsigned char>("cpp.unsigned-char");
    builtin<short>("cpp.short");
    builtin<unsigned short>("cpp.unsigned-short");
    builtin<int>("cpp.int");
    builtin<unsigned>("cpp.unsigned-int");
    builtin<long>("cpp.long");
    builtin<unsigned long>("cpp.unsigned-long");
    builtin<long long>("cpp.long-long");
    builtin<unsigned long long>("cpp.unsigned-long-long");
    builtin<float>("cpp.float");
    builtin<double>("cpp.double");
    builtin<std::string>("cpp.string");
    builtin<serialization::Bytes>("cpp.bytes");
    builtin<std::nullptr_t>("cpp.null");
  }
  template <serialization::Serializable T>
    requires std::copy_constructible<T>
  [[nodiscard]] serialization::Result<void>
  register_type(std::string id, std::uint32_t version = 1) {
    using namespace serialization;
    if (id.empty() || id.size() > 128 || !version)
      return error<void>(ErrorCode::schema_mismatch,
                         "Stable ID or version is invalid");
    for (unsigned char c : id)
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
            c == '/'))
        return error<void>(ErrorCode::schema_mismatch,
                           "Stable ID must use ASCII identifier characters");
    try {
      std::unique_lock lock(mutex_);
      for (const auto &entry : entries_)
        if (entry.id == id || entry.type == typeid(T))
          return error<void>(ErrorCode::duplicate_key,
                             "Wire ID or C++ type is already registered");
      if (entries_.size() >= capacity_)
        return error<void>(ErrorCode::limit_exceeded,
                           "Dynamic codec registry is full");
      entries_.push_back(
          Entry{std::move(id), typeid(T), version,
                [](Encoder &out, const DynamicValue &value) -> Result<void> {
                  return out.write(std::any_cast<const T &>(value));
                },
                [](Decoder &in) -> Result<DynamicValue> {
                  auto result = in.read<T>();
                  if (!result)
                    return std::unexpected(result.error());
                  return DynamicValue{std::move(*result)};
                }});
      return {};
    } catch (...) {
      return error<void>(ErrorCode::codec_failed, "Codec registration threw",
                         std::current_exception());
    }
  }
  [[nodiscard]] serialization::Result<serialization::Bytes>
  serialize(const DynamicValue &value,
            serialization::Limits limits = {}) const {
    using namespace serialization;
    try {
      Encoder out(limits);
      if (!value.has_value()) {
        if (auto r = out.array(0); !r)
          return std::unexpected(r.error());
        return out.finish();
      }
      auto entry = find(value.type());
      if (!entry)
        return std::unexpected(entry.error());
      if (auto r = out.array(3); !r)
        return std::unexpected(r.error());
      if (auto r = out.write(entry->id); !r)
        return std::unexpected(r.error());
      if (auto r = out.write(entry->version); !r)
        return std::unexpected(r.error());
      if (auto r = entry->encode(out, value); !r)
        return std::unexpected(r.error());
      return out.finish();
    } catch (...) {
      return error<Bytes>(ErrorCode::codec_failed, "Dynamic encoding threw",
                          std::current_exception());
    }
  }
  [[nodiscard]] serialization::Result<DynamicValue>
  deserialize(std::span<const std::byte> bytes,
              serialization::Limits limits = {}) const {
    using namespace serialization;
    if (auto r = validate(bytes, limits); !r)
      return std::unexpected(r.error());
    Decoder in(bytes, limits);
    try {
      auto size = in.array();
      if (!size)
        return std::unexpected(size.error());
      if (!*size)
        return DynamicValue{};
      if (*size != 3)
        return in.reject<DynamicValue>(
            ErrorCode::schema_mismatch,
            "Dynamic envelope must have three fields");
      auto id = in.read<std::string>();
      if (!id)
        return std::unexpected(id.error());
      auto version = in.read<std::uint32_t>();
      if (!version)
        return std::unexpected(version.error());
      auto entry = find(*id);
      if (!entry)
        return std::unexpected(entry.error());
      if (*version != entry->version)
        return in.reject<DynamicValue>(ErrorCode::schema_mismatch,
                                       "Dynamic codec version mismatch");
      auto result = entry->decode(in);
      if (!result)
        return std::unexpected(result.error());
      if (auto r = in.finish(); !r)
        return std::unexpected(r.error());
      // In-place construction also supports older libc++ expected<any>.
      return Result<DynamicValue>{std::in_place, std::move(*result)};
    } catch (...) {
      return std::unexpected(Error{ErrorCode::codec_failed, in.position(),
                                   "Dynamic decoding threw",
                                   std::current_exception()});
    }
  }

private:
  struct Entry final {
    std::string id;
    std::type_index type;
    std::uint32_t version;
    serialization::Result<void> (*encode)(serialization::Encoder &,
                                          const DynamicValue &);
    serialization::Result<DynamicValue> (*decode)(serialization::Decoder &);
  };
  const std::size_t capacity_;
  mutable std::shared_mutex mutex_;
  std::vector<Entry> entries_;
  template <class T>
  static serialization::Result<T> error(serialization::ErrorCode code,
                                        const char *message,
                                        std::exception_ptr cause = {}) {
    return std::unexpected(
        serialization::Error{code, 0, message, std::move(cause)});
  }
  template <class T> void builtin(const char *id) {
    if (!register_type<T>(id))
      throw std::invalid_argument(
          "DynamicSerializer capacity cannot hold builtin codecs");
  }
  template <class Key> serialization::Result<Entry> find(const Key &key) const {
    std::shared_lock lock(mutex_);
    for (const auto &entry : entries_) {
      if constexpr (std::same_as<Key, std::string>) {
        if (entry.id == key)
          return entry;
      } else if (entry.type == key)
        return entry;
    }
    return error<Entry>(serialization::ErrorCode::unknown_type,
                        "Dynamic value type is not registered");
  }
};
} // namespace ii::runtime
