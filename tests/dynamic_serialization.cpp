#include "serialization_record.h"
#include <atomic>
#include <filesystem>
#include <iiSDKCore/Runtime/Runtime.h>
#include <iiSDKCore/Runtime/Serialization.h>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace ii::serialization;
using ii::runtime::DynamicSerializer;
using ii::runtime::DynamicValue;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Throwing final {};
struct Broken final {};
struct Owned final {
  static inline std::atomic<int> live = 0;
  Owned() { ++live; }
  Owned(const Owned &) { ++live; }
  Owned(Owned &&) noexcept { ++live; }
  ~Owned() { --live; }
};
namespace ii::serialization {
template <> struct Codec<Throwing> final {
  static Result<void> encode(Encoder &, const Throwing &) {
    throw std::runtime_error("codec");
  }
  static Result<Throwing> decode(Decoder &) {
    throw std::runtime_error("codec");
  }
};
template <> struct Codec<Broken> final {
  static Result<void> encode(Encoder &out, const Broken &) {
    (void)out.write(1);
    (void)out.write(2);
    return {};
  }
  static Result<Broken> decode(Decoder &in) {
    (void)in.read<bool>();
    return Broken{};
  }
};
template <> struct Codec<Owned> final {
  static Result<void> encode(Encoder &out, const Owned &) {
    return out.write(0);
  }
  static Result<Owned> decode(Decoder &in) {
    auto x = in.read<int>();
    if (!x)
      return std::unexpected(x.error());
    return Owned{};
  }
};
} // namespace ii::serialization
int main() {
  try {
    DynamicSerializer serializer;
    auto verify = [&]<class T>(const T &value) {
      auto bytes = serializer.serialize(DynamicValue{value});
      check(bool(bytes), "dynamic encode");
      auto decoded = serializer.deserialize(*bytes);
      check(decoded && decoded->type() == typeid(T) &&
                std::any_cast<T>(*decoded) == value,
            "exact dynamic type roundtrip");
    };
    verify(true);
    verify(char{'a'});
    verify(std::int8_t{-128});
    verify(std::uint8_t{255});
    verify(short{-100});
    verify(100);
    verify(100u);
    verify(-100L);
    verify(100UL);
    verify(-100LL);
    verify(100ULL);
    verify(1.5f);
    verify(1.1);
    verify(std::string("서울"));
    verify(Bytes{std::byte{0}, std::byte{255}});
    verify(nullptr);
    auto empty = serializer.serialize(DynamicValue{});
    check(empty && *empty == Bytes{std::byte{0x80}} &&
              !serializer.deserialize(*empty)->has_value(),
          "empty any differs from explicit null");
    check(bool(serializer.register_type<StoredSample>("app.sample", 1)),
          "register custom type");
    StoredSample record{7, "capture", {1.0f, 0.5f}};
    verify(record);
    check(!serializer.register_type<StoredSample>("app.other", 1),
          "type collision rejected");
    check(!serializer.register_type<std::vector<int>>("app.sample", 1),
          "ID collision rejected");
    check(!serializer.register_type<std::vector<int>>(" bad ", 1),
          "invalid stable ID rejected");
    check(!serializer.serialize(DynamicValue{std::ref(record)}),
          "borrowed references never serialized implicitly");
    check(!serializer.serialize(DynamicValue{1.0L}),
          "long double ABI dependent precision rejected");
    DynamicSerializer receiver(false);
    auto recordBytes = serializer.serialize(DynamicValue{record});
    auto missing = receiver.deserialize(*recordBytes);
    check(!missing && missing.error().code == ErrorCode::unknown_type,
          "unknown type is an error");
    check(bool(receiver.register_type<StoredSample>("app.sample", 2)),
          "different schema registration");
    auto wrong = receiver.deserialize(*recordBytes);
    check(!wrong && wrong.error().code == ErrorCode::schema_mismatch,
          "schema version mismatch");
    DynamicSerializer bounded(false, 1);
    check(bool(bounded.register_type<int>("app.a")), "first capacity slot");
    check(!bounded.register_type<float>("app.b"), "registry capacity limit");
    check(bool(serializer.register_type<Throwing>("app.throw")),
          "throw codec registration");
    auto thrown = serializer.serialize(DynamicValue{Throwing{}});
    check(!thrown && thrown.error().code == ErrorCode::codec_failed &&
              thrown.error().cause,
          "codec exception metadata");
    check(bool(serializer.register_type<Broken>("app.broken")),
          "broken codec registration");
    auto broken = serializer.serialize(DynamicValue{Broken{}});
    check(!broken && broken.error().code == ErrorCode::invalid_format,
          "invalid codec output is rejected");
    auto ownedBytes = serialize(0);
    auto throwingRead = deserialize<Throwing>(*ownedBytes);
    check(!throwingRead &&
              throwingRead.error().code == ErrorCode::codec_failed &&
              throwingRead.error().cause,
          "decoder exception retained");
    {
      auto owned = deserialize<Owned>(*ownedBytes);
      check(owned && Owned::live == 1, "owned decoding");
    }
    check(Owned::live == 0, "all decoded ownership retired");
    auto ownedTrailing = *ownedBytes;
    ownedTrailing.push_back(std::byte{0});
    check(!deserialize<Owned>(ownedTrailing) && Owned::live == 0,
          "failed decode leaves no value");
    auto partialBytes = serialize(std::tuple<int, bool>{0, true});
    check(!deserialize<std::tuple<Owned, int>>(*partialBytes) &&
              Owned::live == 0,
          "partially constructed owned fields retire on failure");
    {
      auto next = deserialize<Owned>(*ownedBytes);
      check(bool(next), "next decode after failure");
    }
    check(Owned::live == 0,
          "next independent decode has no retained reservation");
    check(!deserialize<Broken>(*ownedBytes),
          "ignored reader failure remains sticky");
    if (ii::runtime::Thread::supported()) {
      std::atomic<int> failures = 0;
      ii::runtime::Runtime workers({.workers = 4, .capacity = 16});
      for (int n = 0; n < 4; ++n) {
        auto submission = workers.post([&] {
          for (int i = 0; i < 100; ++i) {
            auto bytes = serializer.serialize(DynamicValue{record});
            if (!bytes) {
              ++failures;
              continue;
            }
            auto output = serializer.deserialize(*bytes);
            if (!output || std::any_cast<StoredSample>(*output) != record)
              ++failures;
          }
        });
        check(bool(submission), "concurrent codec admission");
      }
      check(bool(workers.wait_idle()) && failures == 0 && bool(workers.join()),
            "concurrent per-instance codecs");
    }

    std::cout << "dynamic serialization contracts complete\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
