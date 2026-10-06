#include "serialization_record.h"
#include <iostream>
#include <random>
#include <stdexcept>
using namespace ii::serialization;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
Bytes hex(std::string_view text) {
  Bytes result;
  auto digit = [](char x) { return x <= '9' ? x - '0' : x - 'a' + 10; };
  for (std::size_t i = 0; i < text.size(); i += 2)
    result.push_back(std::byte((digit(text[i]) << 4) | digit(text[i + 1])));
  return result;
}
template <class T> void golden(const T &value, std::string_view wire) {
  auto encoded = serialize(value);
  check(encoded && *encoded == hex(wire), "RFC/profile golden bytes");
  auto decoded = deserialize<T>(hex(wire));
  check(decoded && *decoded == value, "golden decode");
}
template <class T>
void failure(std::string_view wire, ErrorCode code, Limits limits = {}) {
  auto result = deserialize<T>(hex(wire), limits);
  check(!result && result.error().code == code, "decode error category");
  check(result.error().offset <= hex(wire).size(), "error offset in input");
}
int main() {
  try {
    static_assert(Serializable<int> && Serializable<StoredSample>);
    static_assert(!Serializable<int *> && !Serializable<std::string_view> &&
                  !Serializable<long double>);
    golden(std::uint64_t{0}, "00");
    golden(std::uint64_t{23}, "17");
    golden(std::uint64_t{24}, "1818");
    golden(std::uint64_t{256}, "190100");
    golden(std::uint64_t{1000000}, "1a000f4240");
    golden(std::numeric_limits<std::uint64_t>::max(), "1bffffffffffffffff");
    golden(std::int64_t{-1}, "20");
    golden(std::int64_t{-1000}, "3903e7");
    golden(std::numeric_limits<std::int64_t>::min(), "3b7fffffffffffffff");
    golden(true, "f5");
    golden(false, "f4");
    golden(nullptr, "f6");
    golden(std::string("I\0O", 3), "6349004f");
    golden(std::string("한"), "63ed959c");
    golden(hex("010203"), "43010203");
    golden(0.0, "f90000");
    golden(1.5, "f93e00");
    golden(65504.0, "f97bff");
    golden(std::ldexp(1.0, -24), "f90001");
    golden(100000.0, "fa47c35000");
    golden(1.1, "fb3ff199999999999a");
    check(*serialize(-0.0) == hex("f98000") &&
              std::signbit(*deserialize<double>(hex("f98000"))),
          "negative zero");
    check(*serialize(std::numeric_limits<double>::quiet_NaN()) == hex("f97e00"),
          "canonical NaN");
    check(std::isnan(*deserialize<double>(hex("fb7ff8000000000001"))),
          "binary64 NaN input");
    golden(std::numeric_limits<double>::infinity(), "f97c00");
    golden(std::vector<int>{1, 2, 3}, "83010203");
    golden(std::array<int, 2>{-1, 24}, "82201818");
    golden(std::map<std::string, int>{{"aa", 2}, {"b", 1}}, "a261620162616102");
    golden(std::optional<int>{}, "80");
    golden(std::optional<int>{3}, "8103");
    golden(std::optional<std::optional<int>>{std::optional<int>{}}, "8180");
    golden(std::tuple<int, std::string, bool>{7, "x", true}, "83076178f5");
    golden(std::pair<int, int>{1, 2}, "820102");
    golden(std::variant<int, std::string>{std::in_place_index<1>, "hi"},
           "8201626869");
    golden(std::tuple<>{}, "80");
    golden(std::vector<bool>{true, false}, "82f5f4");
    golden(std::array<std::byte, 2>{std::byte{1}, std::byte{2}}, "420102");
    golden(std::unordered_map<std::string, int>{{"aa", 2}, {"b", 1}},
           "a261620162616102");
    golden(std::variant<std::monostate, int>{}, "8200f6");
    Limits shallow;
    shallow.max_depth = 2;
    auto pairBytes = serialize(std::pair{1, 2}, shallow);
    check(pairBytes && deserialize<std::pair<int, int>>(*pairBytes, shallow) ==
                           std::pair{1, 2},
          "pair encode/decode nesting symmetry");
    for (unsigned bits = 0; bits <= 65535; ++bits) {
      Bytes half{std::byte{0xf9}, std::byte(bits >> 8), std::byte(bits)};
      auto value = deserialize<double>(half);
      auto canonical = serialize(*value);
      if (std::isnan(*value))
        check(*canonical == hex("f97e00"), "all NaNs canonicalized");
      else
        check(*canonical == half, "all binary16 values preserve exact bits");
    }
    auto unsortedInput =
        deserialize<std::map<std::string, int>>(hex("a262616102616201"));
    check(unsortedInput && serialize(*unsortedInput) == hex("a261620162616102"),
          "unsorted input normalizes");
    Encoder unsorted;
    check(bool(unsorted.map(2)) && bool(unsorted.text("aa")) &&
              bool(unsorted.write(2)) && bool(unsorted.text("b")) &&
              bool(unsorted.write(1)),
          "manual map construction");
    auto orderFailure = unsorted.finish();
    check(!orderFailure &&
              orderFailure.error().code == ErrorCode::invalid_format,
          "custom map output must be deterministic");
    Encoder duplicate;
    check(bool(duplicate.map(2)) && bool(duplicate.text("a")) &&
              bool(duplicate.write(1)) && bool(duplicate.text("a")) &&
              bool(duplicate.write(2)),
          "duplicate map construction");
    auto duplicateFailure = duplicate.finish();
    check(!duplicateFailure &&
              duplicateFailure.error().code == ErrorCode::duplicate_key,
          "custom duplicate output rejected");
    StoredSample sample{42, "live", {0.5f, -1.0f}};
    auto bytes = serialize(sample);
    check(bytes && deserialize<StoredSample>(*bytes) == sample,
          "custom codec owned roundtrip");
    auto legacy = deserialize<StoredSample>(hex("83000781f93c00"));
    check(legacy && *legacy == StoredSample{7, "legacy", {1.0f}},
          "explicit version migration");
    check(*deserialize<std::uint64_t>(hex("1800")) == 0,
          "valid nonpreferred input");
    for (std::size_t n = 0; n < bytes->size(); ++n)
      check(!deserialize<StoredSample>(std::span(*bytes).first(n)),
            "all truncated prefixes rejected");
    failure<int>("", ErrorCode::truncated);
    failure<int>("1aff", ErrorCode::truncated);
    failure<int>("f5", ErrorCode::type_mismatch);
    failure<std::uint32_t>("20", ErrorCode::out_of_range);
    failure<std::int8_t>("1880", ErrorCode::out_of_range);
    failure<int>("3bffffffffffffffff", ErrorCode::out_of_range);
    failure<float>("fb3ff199999999999a", ErrorCode::out_of_range);
    failure<int>("0000", ErrorCode::invalid_format);
    failure<std::string>("62c080", ErrorCode::invalid_utf8);
    failure<std::string>("63eda080", ErrorCode::invalid_utf8);
    failure<std::string>("64f4908080", ErrorCode::invalid_utf8);
    failure<std::string>("61ff", ErrorCode::invalid_utf8);
    failure<int>("1c", ErrorCode::invalid_format);
    failure<int>("c000", ErrorCode::unsupported);
    failure<std::vector<int>>("9f01ff", ErrorCode::unsupported);
    failure<std::map<std::string, int>>("a2616101616102",
                                        ErrorCode::duplicate_key);
    failure<std::map<std::string, int>>("a10101", ErrorCode::type_mismatch);
    failure<std::array<int, 2>>("8101", ErrorCode::schema_mismatch);
    failure<std::variant<int, std::string>>("820203",
                                            ErrorCode::schema_mismatch);
    failure<StoredSample>("8402182a616180", ErrorCode::schema_mismatch);
    Limits small;
    small.max_bytes = 2;
    failure<std::vector<int>>("820102", ErrorCode::limit_exceeded, small);
    check(!serialize(std::string("hello"), small), "bounded output");
    small = Limits{};
    small.max_blob_bytes = 2;
    failure<std::string>("63616263", ErrorCode::limit_exceeded, small);
    small = Limits{};
    small.max_container_items = 1;
    failure<std::vector<int>>("820102", ErrorCode::limit_exceeded, small);
    small = Limits{};
    small.max_items = 2;
    failure<std::vector<int>>("820102", ErrorCode::limit_exceeded, small);
    small = Limits{};
    small.max_depth = 2;
    failure<std::vector<std::vector<int>>>("818100", ErrorCode::limit_exceeded,
                                           small);
    small = Limits{};
    small.max_allocation_bytes = 1;
    failure<std::vector<int>>("8100", ErrorCode::limit_exceeded, small);
    failure<std::vector<int>>("9bffffffffffffffff", ErrorCode::limit_exceeded);
    failure<Bytes>("5bffffffffffffffff", ErrorCode::limit_exceeded);
    check(!serialize(std::string("\xff", 1)),
          "invalid UTF8 rejected on encode");
    std::mt19937_64 random(42);
    for (int i = 0; i < 2000; ++i) {
      auto integer = random();
      auto encoded = serialize(integer);
      check(encoded && deserialize<std::uint64_t>(*encoded) == integer,
            "random uint64 roundtrip");
      auto real = std::bit_cast<double>(random());
      auto realBytes = serialize(real);
      auto result = deserialize<double>(*realBytes);
      check(result &&
                (std::isnan(real) ? std::isnan(*result)
                                  : std::bit_cast<std::uint64_t>(*result) ==
                                        std::bit_cast<std::uint64_t>(real)),
            "random float bit/sign roundtrip");
      Bytes malformed(std::size_t(random() % 40));
      for (auto &byte : malformed)
        byte = std::byte(random());
      (void)deserialize<std::map<std::string, std::vector<double>>>(malformed);
    }
    std::cout << "serialization contracts complete\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
