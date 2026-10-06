#include <array>
#include <cmath>
#include <fstream>
#include <iiSDKCore/Animation/Timeline.h>
#include <iiSDKCore/Core/Event/Receiver.h>
#include <iiSDKCore/Core/Memory/SharedValue.h>
#include <iiSDKCore/Core/Task/Parallel.h>
#include <iiSDKCore/IO/AtomicFile.h>
#include <iiSDKCore/Plugin/Plugin.h>
#include <iiSDKCore/Runtime/Animation.h>
#include <iostream>
#include <latch>
#include <map>
#include <memory_resource>
#include <numeric>
#include <ranges>

using namespace ii;
using namespace std::chrono_literals;
void check(bool v, const char *m) {
  if (!v)
    throw std::runtime_error(m);
}

void containers() {
  std::array<std::byte, 4096> storage{};
  std::pmr::monotonic_buffer_resource arena(storage.data(), storage.size(),
                                            std::pmr::null_memory_resource());
  for (int iteration = 0; iteration < 1000; ++iteration) {
    {
      std::pmr::vector<int> values(&arena);
      values.reserve(64);
      for (int i = 0; i < 64; ++i)
        values.push_back(i);
      auto evens =
          values | std::views::filter([](int x) { return x % 2 == 0; });
      check(std::ranges::distance(evens) == 32,
            "standard container/range composition");
      std::pmr::map<int, int> index(&arena);
      index.emplace(1, values[1]);
      check(index.at(1) == 1, "standard associative container");
    }
    arena.release(); // only after every container has been destroyed
  }
  bool full = false;
  try {
    std::pmr::vector<int> over(&arena);
    over.resize(4096);
  } catch (const std::bad_alloc &) {
    full = true;
  }
  check(full, "fixed memory budget has no heap fallback");
}
void values_and_animation() {
  core::SharedValue<std::vector<int>> value(std::vector{1, 2});
  auto copy = value;
  auto old = value.snapshot();
  check(bool(value.update([](auto &x) { x.push_back(3); })), "shared update");
  check(old.get().size() == 2 && copy.snapshot().get().size() == 2 &&
            value.snapshot().get().size() == 3,
        "snapshot isolation");
  auto failed = value.update([](auto &x) {
    x.clear();
    throw std::runtime_error("failure");
  });
  check(!failed && value.snapshot().get().size() == 3,
        "transaction failure preserves data");
  check(!value.update([&](auto &) {
    check(bool(value.update([](auto &x) { x[0] = 9; })), "inner update");
  }),
        "optimistic conflict");
  animation::Keyframes<double> curve({{0ms, 0.0}, {100ms, 1.0}, {200ms, 0.0}});
  check(curve.sample(50ms) == 0.5 && curve.sample(250ms) == 0.0,
        "keyframes interpolation and bounds");
  check(std::abs(animation::ease(animation::Easing::smooth, 0.25) - 0.15625) <
            1e-9,
        "easing");
  animation::Timeline timeline;
  double a = -1, b = -1;
  timeline.append(100ms, [&](double p) { a = p; });
  timeline.append(100ms, [&](double p) { b = p; });
  timeline.parallel(0ms, 200ms, [&](double p) {
    check(p >= 0 && p <= 1, "parallel normalized");
  });
  timeline.sample(50ms);
  check(a == 0.5 && b == -1 && timeline.duration() == 200ms,
        "sequential and parallel tracks");
  timeline.sample(200ms);
  check(a == 1 && b == 1, "terminal samples");
  runtime::EventLoop loop(4);
  runtime::Animation playback(loop, std::move(timeline), {.interval = 1ms});
  check(bool(playback.play()) && loop.poll() == 1,
        "animation schedules first frame");
  check(bool(playback.pause()), "animation pauses");
  check(bool(playback.seek(200ms)) && a == 1 && b == 1, "paused seeking");
  check(bool(playback.close()) && loop.stats().pending == 0,
        "animation cancels and returns quota");
  check(!playback.play(), "closed playback admission");
  bool invalid = false;
  try {
    animation::Keyframes<double> bad({{1ms, 0}, {1ms, 1}});
  } catch (const std::invalid_argument &) {
    invalid = true;
  }
  check(invalid, "duplicate keyframe rejected");
}

void animation_removal() {
  runtime::EventLoop loop(8);
  animation::Timeline timeline;
  runtime::Animation *playback = nullptr;
  auto capture = std::make_shared<int>(1);
  std::weak_ptr<int> weak = capture;
  timeline.append(1ms, [&, owned = capture](double) {
    check(playback->state() == runtime::AnimationState::playing,
          "callback status observation");
    check(!playback->pause() && playback->close().error().code ==
                                    runtime::ThreadErrorCode::self_join,
          "recursive animation control rejected");
  });
  runtime::Animation instance(loop, std::move(timeline));
  playback = &instance;
  capture.reset();
  check(bool(instance.play()) && loop.poll() == 1, "callback playback");
  check(!weak.expired() && bool(instance.close()) && weak.expired() &&
            loop.stats().pending == 0,
        "animation captures retire before quota reuse");
  animation::Timeline bad;
  bad.append(1ms, [](double) { throw std::runtime_error("sample failed"); });
  runtime::Animation broken(loop, std::move(bad));
  check(bool(broken.play()) && loop.poll() == 1 &&
            broken.state() == runtime::AnimationState::failed &&
            broken.failure(),
        "animation failure retained");
  check(bool(broken.close()), "failed animation closes");
  if (!runtime::Thread::supported())
    return;
  std::latch entered(1), release(1), closing(1);
  animation::Timeline active;
  active.append(1s, [&](double) {
    entered.count_down();
    release.wait();
  });
  runtime::Animation live(loop, std::move(active));
  runtime::Thread driver;
  check(bool(live.play()) && bool(driver.start(loop)),
        "active animation driver");
  entered.wait();
  auto removal = std::async(std::launch::async, [&] {
    closing.count_down();
    return live.close();
  });
  closing.wait();
  check(removal.wait_for(1ms) != std::future_status::ready,
        "removal waits active frame");
  release.count_down();
  check(bool(removal.get()) && loop.stats().pending == 0,
        "active animation removal completes");
  driver.request_stop();
  check(bool(driver.join()), "animation driver joins");
}
void parallel_and_events() {
  if (!runtime::Thread::supported())
    return;
  runtime::Runtime workers({.workers = 2, .capacity = 16});
  auto empty = core::parallel_transform(workers, std::vector<int>{},
                                        [](int x) { return x; });
  check(empty.completion.get() && empty.completion.get()->empty(),
        "empty transform settles");
  auto zero = core::parallel_transform(
      workers, std::vector{1}, [](int x) { return x; }, 0);
  check(!zero.completion.get() && zero.completion.get().error().code ==
                                      core::ErrorCode::invalid_argument,
        "zero grain rejected");
  std::vector<int> input(1000);
  std::iota(input.begin(), input.end(), 0);
  auto mapped = core::parallel_transform(
      workers, std::move(input), [](int x) { return x * x; }, 128);
  auto result = mapped.completion.get();
  check(result && result->size() == 1000 && (*result)[999] == 998001,
        "ordered parallel transform");
  auto failed = core::parallel_transform(
      workers, std::vector{1, 2, 3},
      [](int x) -> int {
        if (x == 2)
          throw std::runtime_error("map failure");
        return x;
      },
      1);
  check(!failed.completion.get(), "parallel exceptions settle future");
  std::latch started(1), release(1);
  runtime::Runtime blocked({.workers = 1, .capacity = 2});
  auto running = blocked.post([&] {
    started.count_down();
    release.wait();
  });
  started.wait();
  auto cancelled = core::parallel_transform(
      blocked, std::vector{1, 2, 3}, [](int x) { return x; }, 3);
  cancelled.cancel();
  auto cancelled_result = cancelled.completion.get();
  check(!cancelled_result &&
            cancelled_result.error().code == core::ErrorCode::cancelled,
        "queued parallel cancellation settles future");
  auto rejected = core::parallel_transform(
      blocked, std::vector{1, 2, 3}, [](int x) { return x; }, 1);
  check(!rejected.completion.get(), "partial admission failure settles future");
  release.count_down();
  check(running && bool(blocked.join()), "blocked worker release");
  std::latch editing(1), commit(1);
  core::SharedValue<int> shared(1);
  auto edit = std::async(std::launch::async, [&] {
    return shared.update([&](int &x) {
      editing.count_down();
      commit.wait();
      x = 10;
    });
  });
  editing.wait();
  check(bool(shared.update([](int &x) { x = 20; })), "concurrent commit");
  commit.count_down();
  check(!edit.get() && shared.snapshot().get() == 20,
        "concurrent stale update rejected");
  runtime::SerialQueue queue(workers);
  int total = 0;
  core::EventSequence sequence;
  core::EventReceiver<int> receiver(
      queue, [&](const auto &e) { total += e.payload; },
      [](const auto &e) { return e.payload > 0; });
  std::promise<bool> filtered, delivered;
  check(bool(receiver.post(
            sequence.make(-2),
            [&](auto reply) { filtered.set_value(reply && !*reply); })),
        "filter admission");
  check(bool(receiver.post(
            sequence.make(3),
            [&](auto reply) { delivered.set_value(reply && *reply); })),
        "event admission");
  check(filtered.get_future().get() && delivered.get_future().get() &&
            total == 3,
        "filters run in serial domain");
  check(bool(receiver.close()) && !receiver.post(sequence.make(1), [](auto) {}),
        "receiver close cancels admission");
  check(bool(queue.close()) && bool(workers.join()), "parallel workers clean");
}

void files() {
  auto path = std::filesystem::current_path() / "modern-atomic-test.dat";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  const std::array<std::byte, 3> bytes{std::byte{1}, std::byte{2},
                                       std::byte{3}};
  auto file = io::AtomicFile::create(path);
  check(bool(file), "atomic create");
  auto temporary = file->temporary_path();
  check(bool(file->write(bytes)) && !std::filesystem::exists(path),
        "uncommitted target untouched");
  check(bool(file->commit()) && !std::filesystem::exists(temporary),
        "atomic commit retires temporary");
  auto replacement = io::AtomicFile::create(path);
  check(replacement && bool(replacement->write(std::span(bytes).first(1))),
        "replacement admission");
  temporary = replacement->temporary_path();
  check(bool(replacement->cancel()) && !std::filesystem::exists(temporary) &&
            std::filesystem::file_size(path) == 3,
        "cancel preserves original");
  auto again = io::AtomicFile::create(path);
  check(again && bool(again->write(std::span(bytes).first(1))) &&
            bool(again->commit()) && std::filesystem::file_size(path) == 1,
        "replace existing file");
  std::filesystem::remove(path, ec);
}
int main() {
  try {
    containers();
    values_and_animation();
    animation_removal();
    parallel_and_events();
    files();
    std::cout << "modern core contracts complete\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
