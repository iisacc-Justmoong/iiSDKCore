#include <atomic>
#include <iiSDKCore/Core/Serialization/SharedValue.h>
#include <iostream>
#include <stdexcept>
using ii::core::SharedValue;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Tracked final {
  static inline std::atomic<int> copies = 0, live = 0;
  static inline bool throw_copy = false;
  int value;
  explicit Tracked(int value) : value(value) { ++live; }
  Tracked(const Tracked &other) : value(other.value) {
    if (throw_copy)
      throw std::runtime_error("copy rejected");
    ++copies;
    ++live;
  }
  Tracked(Tracked &&other) noexcept : value(other.value) { ++live; }
  ~Tracked() { --live; }
};
int main() {
  try {
    {
      SharedValue<Tracked> original(Tracked{1});
      Tracked::copies = 0;
      auto copy = original;
      SharedValue<Tracked> assigned(Tracked{9});
      assigned = original;
      check(Tracked::copies == 0, "copy/assignment share without copying T");
      auto old = original.snapshot();
      check(old.value == copy.snapshot().value &&
                old.value == assigned.snapshot().value,
            "same initial shared allocation");
      check(bool(copy.modify([](Tracked &value) noexcept { value.value = 2; })),
            "automatic detach");
      check(Tracked::copies == 1 && copy.snapshot().get().value == 2 &&
                old.get().value == 1 && assigned.snapshot().get().value == 1,
            "one shared write detaches once and isolates observers");
    }
    check(Tracked::live == 0, "last strong owner destroys storage");
    {
      SharedValue<Tracked> unique(Tracked{1});
      Tracked::copies = 0;
      for (int i = 0; i < 1000; ++i)
        check(
            bool(unique.modify([](Tracked &value) noexcept { ++value.value; })),
            "unique write");
      check(Tracked::copies == 0 && unique.snapshot().get().value == 1001,
            "unique noexcept writes do not copy T");
      auto revision = unique.snapshot().revision;
      auto failed = unique.modify([](Tracked &value) {
        value.value = -1;
        throw std::runtime_error("edit rejected");
      });
      check(!failed && failed.error().cause &&
                unique.snapshot().get().value == 1001 &&
                unique.snapshot().revision == revision,
            "throwing write rolls back value and revision");
      check(bool(unique.modify([&](Tracked &value) {
              auto nested = unique.modify([](Tracked &) noexcept {});
              check(!nested && nested.error().code == ii::core::ErrorCode::busy,
                    "recursive writer rejected");
              auto update = unique.update([](Tracked &) {});
              check(!update && update.error().code == ii::core::ErrorCode::busy,
                    "recursive transactional writer rejected");
              bool blocked = false;
              try {
                (void)unique.snapshot();
              } catch (const std::logic_error &) {
                blocked = true;
              }
              check(blocked, "recursive read rejected without deadlock");
              ++value.value;
            })),
            "outer write commits after handled reentry");
      auto retained = unique.snapshot();
      Tracked::throw_copy = true;
      auto rejected =
          unique.modify([](Tracked &value) noexcept { value.value = 7; });
      Tracked::throw_copy = false;
      check(!rejected && unique.snapshot().value == retained.value &&
                unique.snapshot().revision == retained.revision,
            "copy failure leaves original allocation intact");
    }
    check(Tracked::live == 0, "failed candidate releases all owned objects");
    std::weak_ptr<const Tracked> weak;
    {
      SharedValue<Tracked> value(Tracked{3});
      {
        auto published = value.snapshot();
        weak = published.value;
      }
      Tracked::copies = 0;
      check(bool(value.modify([](Tracked &x) noexcept { x.value = 4; })),
            "write after observation");
      check(Tracked::copies == 1 && weak.expired(),
            "formerly published allocation is never modified through weak "
            "aliases");
      Tracked::copies = 0;
      check(bool(value.modify([](Tracked &x) noexcept { x.value = 5; })) &&
                Tracked::copies == 0,
            "new private storage is reusable");
    }
    check(Tracked::live == 0 && weak.expired(),
          "weak reference does not retain payload");
    namespace wire = ii::serialization;
    SharedValue<std::vector<int>> source(std::vector{1, 2, 3});
    auto bytes = wire::serialize(source);
    check(bytes && *bytes == *wire::serialize(std::vector{1, 2, 3}),
          "shared wrapper uses logical value wire format");
    auto restored = wire::deserialize<SharedValue<std::vector<int>>>(*bytes);
    check(restored && restored->snapshot().get() == source.snapshot().get() &&
              restored->snapshot().revision == 0,
          "decoded value gets fresh independent storage");
    auto restoredCopy = *restored;
    check(
        bool(restoredCopy.modify([](auto &value) noexcept { value[0] = 9; })) &&
            restored->snapshot().get()[0] == 1,
        "decoded values retain implicit sharing");
    wire::Limits limits;
    limits.max_allocation_bytes = sizeof(std::vector<int>) - 1;
    auto bounded = wire::deserialize<SharedValue<std::vector<int>>>(
        *wire::serialize(std::vector<int>{}), limits);
    check(!bounded && bounded.error().code == wire::ErrorCode::limit_exceeded,
          "shared block storage is accounted during decode");
    std::cout << "implicit sharing contracts complete\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
