#include <atomic>
#include <iiSDKCore/Core/Serialization/SharedValue.h>
#include <iiSDKCore/Runtime/Query.h>
#include <iiSDKCore/Runtime/Serialization.h>
#include <iostream>
#include <latch>
#include <stdexcept>
using namespace ii;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Payload final {
  static inline std::atomic<int> copies = 0, live = 0;
  int count;
  serialization::Bytes bytes;
  explicit Payload(int count) : count(count), bytes(65536, std::byte{1}) {
    ++live;
  }
  Payload(int count, serialization::Bytes bytes)
      : count(count), bytes(std::move(bytes)) {
    ++live;
  }
  Payload(const Payload &other) : count(other.count), bytes(other.bytes) {
    ++copies;
    ++live;
  }
  Payload(Payload &&other) noexcept
      : count(other.count), bytes(std::move(other.bytes)) {
    ++live;
  }
  ~Payload() { --live; }
};
namespace ii::serialization {
template <> struct Codec<Payload> final {
  static Result<void> encode(Encoder &out, const Payload &value) {
    if (auto result = out.array(2); !result)
      return result;
    if (auto result = out.write(value.count); !result)
      return result;
    return out.write(value.bytes);
  }
  static Result<Payload> decode(Decoder &in) {
    auto size = in.array();
    if (!size)
      return std::unexpected(size.error());
    if (*size != 2)
      return in.reject<Payload>(ErrorCode::schema_mismatch,
                                "Payload shape mismatch");
    auto count = in.read<int>();
    if (!count)
      return std::unexpected(count.error());
    auto bytes = in.read<Bytes>();
    if (!bytes)
      return std::unexpected(bytes.error());
    return Payload(*count, std::move(*bytes));
  }
};
} // namespace ii::serialization
using Shared = core::SharedValue<Payload>;
void dynamic_values() {
  Shared original(Payload{3});
  Payload::copies = 0;
  runtime::DynamicValue value = original, copy = value;
  check(Payload::copies == 0, "any copies retain implicit sharing");
  auto &edited = std::any_cast<Shared &>(copy);
  check(bool(edited.modify([](Payload &x) noexcept {
          x.count = 4;
          x.bytes[0] = std::byte{9};
        })),
        "dynamic shared write");
  check(Payload::copies == 1 && original.snapshot().get().count == 3 &&
            edited.snapshot().get().count == 4,
        "dynamic copy isolates writes");
  runtime::DynamicSerializer serializer;
  check(bool(serializer.register_type<Shared>("app.shared-payload")),
        "shared dynamic codec registration");
  auto bytes = serializer.serialize(copy);
  auto result = serializer.deserialize(*bytes);
  check(result && result->type() == typeid(Shared) &&
            std::any_cast<const Shared &>(*result).snapshot().get().count == 4,
        "dynamic serialization restores shared value contract");
}
void worker_queries() {
  runtime::Runtime workers({.workers = 4, .capacity = 16});
  {
    Shared input(Payload{1});
    Payload::copies = 0;
    runtime::SerialQueue queue(workers);
    runtime::QueryPort<Shared, core::Result<Shared>> port(
        queue, [](const Shared &request) -> core::Result<Shared> {
          auto response = request;
          if (auto result =
                  response.modify([](Payload &x) noexcept { x.count = 2; });
              !result)
            return std::unexpected(result.error());
          return response;
        });
    bool correct = false;
    auto admission = port.post(input, [&](auto reply) {
      correct = reply && *reply && (**reply).snapshot().get().count == 2;
    });
    check(admission && workers.wait_idle() && correct,
          "asynchronous shared payload query");
    check(Payload::copies == 1 && input.snapshot().get().count == 1,
          "query payload copies are cheap; only receiver write copies data");
    check(port.close() && queue.close(), "query resources retired");
  }
  {
    Shared value(Payload{0});
    std::weak_ptr<const Payload> weak = value.snapshot().value;
    std::atomic<bool> entered = false, release = false;
    bool success = false;
    Payload::copies = 0;
    auto task = workers.post([&] {
      success = bool(value.modify([&](Payload &x) noexcept {
        x.count = 17;
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
          std::this_thread::yield();
        x.count = 18;
      }));
    });
    check(bool(task), "weak alias writer admission");
    while (!entered.load(std::memory_order_acquire))
      std::this_thread::yield();
    auto observed = weak.lock();
    const bool stable = observed && observed->count == 0;
    release.store(true, std::memory_order_release);
    check(workers.wait_idle() && success && stable && Payload::copies == 1,
          "weak alias upgrade during unique write sees immutable old data");
    check(value.snapshot().get().count == 18 && observed->count == 0,
          "published snapshot survives commit");
  }
  {
    core::SharedValue<int> counter(0);
    for (int n = 0; n < 4; ++n)
      check(bool(workers.post([&] {
              for (int i = 0; i < 500; ++i)
                if (!counter.modify([](int &x) noexcept { ++x; }))
                  throw std::runtime_error("counter write failed");
            })),
            "counter admission");
    check(workers.wait_idle() && counter.snapshot().get() == 2000 &&
              counter.snapshot().revision == 2000,
          "serialized same-instance writes preserve every update");
    std::latch editing(1), commit(1);
    bool conflicted = false;
    check(bool(workers.post([&] {
            auto result = counter.update([&](int &x) {
              editing.count_down();
              commit.wait();
              x = -1;
            });
            conflicted =
                !result && result.error().code == core::ErrorCode::busy;
          })),
          "optimistic edit admission");
    editing.wait();
    auto modification = counter.modify([](int &x) noexcept { ++x; });
    commit.count_down();
    check(modification && workers.wait_idle() && conflicted &&
              counter.snapshot().get() == 2001,
          "implicit write invalidates stale optimistic commit");
  }
  check(bool(workers.join()), "persistent workers joined");
}
int main() {
  try {
    dynamic_values();
    check(Payload::live == 0, "dynamic values release shared storage");
    if (runtime::Thread::supported())
      worker_queries();
    check(Payload::live == 0,
          "queries, snapshots and weak aliases release payloads");
    std::cout << "implicit sharing runtime complete\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
