#include <atomic>
#include <iiSDKCore/Core/Time/Time.h>
#include <iiSDKCore/Runtime/EventLoop.h>
#include <iiSDKCore/Runtime/LiveStream.h>
#include <iiSDKCore/Runtime/Thread.h>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace ii::runtime;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char *reason) {
  if (!value)
    throw std::runtime_error(reason);
}
void time_arithmetic() {
  const auto origin = Clock::time_point{};
  auto position = ii::core::Time::period(origin, 10ms, origin + 27ms);
  check(position.release == origin + 20ms && position.next == origin + 30ms &&
            position.skipped == 2,
        "phase preserved without catch-up burst");
  check(ii::core::Time::add(Clock::time_point::max(), 1ms) ==
            Clock::time_point::max(),
        "upper saturation");
  check(ii::core::Time::add(Clock::time_point::min(), -1ms) ==
            Clock::time_point::min(),
        "lower saturation");
  check(ii::core::Time::elapsed(Clock::time_point::min(),
                                Clock::time_point::max()) ==
            Clock::duration::max(),
        "elapsed saturation across signed epochs");
  auto extreme = ii::core::Time::period(
      Clock::time_point::min(), Clock::duration(1), Clock::time_point::max());
  check(extreme.release == Clock::time_point::max() &&
            extreme.next == Clock::time_point::max(),
        "extreme grid arithmetic");
}
void scheduling() {
  EventLoop loop(4);
  RealtimeSchedule plan{100ms, 90ms, 20ms, 2ms, Clock::now()};
  std::vector<int> order;
  auto later = loop.realtime(plan, [&](const ii::core::TimeSlice &) {
    order.push_back(2);
    return SliceResult::complete;
  });
  plan.deadline = 50ms;
  auto earlier = loop.realtime(plan, [&](const ii::core::TimeSlice &) {
    order.push_back(1);
    return SliceResult::complete;
  });
  check(bool(later) && bool(earlier), "admit bounded reservations");
  check(loop.poll({2, 50ms}) == 2 && order == std::vector<int>{1, 2},
        "earliest deadline first");
  check(later.token.cancel() && earlier.token.cancel() &&
            loop.stats().pending == 0,
        "reservations release capacity");
  plan.runtime = 80ms;
  plan.deadline = 90ms;
  auto reserved =
      loop.realtime(plan, [](const auto &) { return SliceResult::complete; });
  check(bool(reserved) &&
            loop.realtime(plan,
                          [](const auto &) { return SliceResult::complete; })
                    .error == SubmitError::overloaded,
        "reject overcommitted runtime reservations");
  check(reserved.token.cancel(), "return runtime reservation");
  plan.runtime = 20ms;
  plan.deadline = 50ms;
  int steps = 0;
  auto yielding = loop.realtime(plan, [&](const ii::core::TimeSlice &slice) {
    check(slice.ends <= slice.deadline && slice.ends > slice.started,
          "bounded slice context");
    ++steps;
    return steps == 3 ? SliceResult::stop : SliceResult::yield;
  });
  check(bool(yielding) && loop.poll({10, 50ms}) == 3 && steps == 3 &&
            !yielding.token.pending(),
        "cooperative continuation and final retirement");
  plan.quantum = 0ns;
  check(loop.realtime(plan, [](const auto &) { return SliceResult::stop; })
                .error == SubmitError::invalid,
        "invalid quantum");
}
void live_stream() {
  EventLoop loop(4);
  LiveStream<int> stream(3, 50ms);
  const auto now = Clock::now();
  check(stream.push(now - 100ms, 0) && stream.push(now, 1) &&
            stream.push(now + 1h, 2),
        "timestamped buffer");
  check(!stream.push(now, 3), "out-of-order timestamp rejected");
  std::vector<int> received;
  RealtimeSchedule plan{10ms, 10ms, 2ms, 1ms, Clock::now()};
  check(bool(stream.start(
            loop, plan,
            [&](TimedFrame<int> frame) { received.push_back(frame.value); })),
        "start timed playout");
  loop.poll({10, 50ms});
  check(received == std::vector<int>{1} && stream.stats().late == 1 &&
            stream.stats().pending == 1,
        "discard stale frame and preserve future frame");
  check(bool(stream.close()) && stream.stats().pending == 0 &&
            loop.stats().pending == 0,
        "clean live stream removal");
  check(!stream.push(now + 2h, 4), "closed producer rejected");
  LiveStream<int> recent(2, 50ms);
  check(recent.push(now, 1) && recent.push(now, 2) && recent.push(now, 3) &&
            recent.stats().dropped == 1,
        "live overflow keeps bounded recent frames");
  check(recent.take(now)->value == 2, "live FIFO after overflow");
  check(bool(recent.close()), "unstarted stream clean close");
}
void continuations_and_removal() {
  EventLoop loop(4);
  RealtimeSchedule plan{1h, 1h, 10ms, 2ms, Clock::now()};
  std::vector<int> order;
  int first = 0, second = 0;
  auto a = loop.realtime(plan, [&](const auto &) {
    order.push_back(1);
    return ++first == 2 ? SliceResult::stop : SliceResult::yield;
  });
  auto b = loop.realtime(plan, [&](const auto &) {
    order.push_back(2);
    return ++second == 2 ? SliceResult::stop : SliceResult::yield;
  });
  check(a && b && loop.poll({10, 100ms}) == 4 &&
            order == std::vector<int>{1, 2, 1, 2},
        "equal deadline continuation rotation");
  plan.start = Clock::now() - 24h;
  int delayed = 0;
  auto old = loop.realtime(plan, [&](const auto &) {
    ++delayed;
    return SliceResult::complete;
  });
  check(old && loop.poll({20, 100ms}) == 1 && delayed == 1 &&
            loop.stats().skipped_releases >= 24,
        "late host skips old releases without callback burst");
  check(old.token.cancel(), "cancel resumed time grid");
  auto failing = loop.realtime(plan, [](const auto &) -> SliceResult {
    throw std::runtime_error("slice failure");
  });
  bool raised = false;
  try {
    loop.poll({10, 100ms});
  } catch (const std::runtime_error &) {
    raised = true;
  }
  check(failing && raised && loop.stats().pending == 0 &&
            loop.stats().reserved_utilization == 0,
        "exception releases callback and runtime quota");
  plan.period = plan.deadline = 100ms;
  plan.runtime = 80ms;
  plan.start = Clock::now() + 1h;
  bool destroyed = false, reserved_during_destruction = false;
  auto resource = std::shared_ptr<int>(new int, [&](int *p) {
    delete p;
    destroyed = true;
    auto nested =
        loop.realtime(plan, [](const auto &) { return SliceResult::stop; });
    reserved_during_destruction = nested.error == SubmitError::overloaded;
    if (nested)
      (void)nested.token.cancel();
  });
  auto held = loop.realtime(
      plan, [resource](const auto &) { return SliceResult::stop; });
  resource.reset();
  check(held && held.token.cancel() && destroyed && reserved_during_destruction,
        "destroy captures before exposing runtime reservation");
  auto reused =
      loop.realtime(plan, [](const auto &) { return SliceResult::stop; });
  check(reused && reused.token.cancel(),
        "reuse reservation after full removal");
}
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
void active_live_removal() {
  EventLoop loop(4);
  LiveStream<std::shared_ptr<int>> stream(2, 1h);
  auto payload = std::make_shared<int>(7);
  std::weak_ptr<int> released = payload;
  check(stream.push(Clock::now(), payload) &&
            stream.push(Clock::now(), payload),
        "owned buffered payloads");
  payload.reset();
  std::latch entered(1), release(1), closing(1);
  std::atomic<bool> closed = false;
  std::atomic<int> received = 0;
  check(bool(stream.start(loop, RealtimeSchedule{},
                          [&](auto) {
                            ++received;
                            check(!stream.close(), "own handler close is busy");
                            entered.count_down();
                            release.wait();
                          })),
        "start active stream");
  Thread dispatcher, closer;
  check(bool(dispatcher.start([&](ThreadContext &) { loop.poll({64, 1s}); })),
        "managed playout dispatcher");
  entered.wait();
  check(bool(closer.start([&](ThreadContext &) {
          closing.count_down();
          check(bool(stream.close()), "foreign clean close");
          closed = true;
        })),
        "managed stream closer");
  closing.wait();
  while (stream.start(loop, RealtimeSchedule{}, [](auto) {}).error !=
         SubmitError::stopped)
    std::this_thread::yield();
  check(!closed, "close waits for active delivery");
  release.count_down();
  check(bool(closer.join()) && bool(dispatcher.join()) && closed &&
            released.expired() && received == 1 && loop.stats().pending == 0,
        "active delivery and buffered captures retire before close completes");
}
void native_driver() {
  EventLoop loop(4);
  std::atomic<int> calls = 0;
  RealtimeSchedule plan{5ms, 5ms, 2ms, 100us, Clock::now()};
  auto work = loop.realtime(plan, [&](const auto &) {
    if (++calls == 1) {
      std::this_thread::sleep_for(3ms);
      return SliceResult::yield;
    }
    loop.request_stop();
    return SliceResult::stop;
  });
  check(bool(work), "native timed reservation");
  loop.run({64, 20ms});
  check(calls == 2 && loop.stats().budget_exhaustions >= 1 &&
            loop.stats().slice_overruns >= 1 && loop.stats().pending == 0,
        "runtime quota throttles until next period and run wakes by time");
}
#endif
} // namespace
int main() {
  try {
    time_arithmetic();
    scheduling();
    live_stream();
    continuations_and_removal();
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    active_live_removal();
    native_driver();
#endif
    std::cout << "realtime scheduling and timestamped playout passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
