#include <atomic>
#include <iiSDKCore/Core/Time/Time.h>
#include <iiSDKCore/Runtime/EventLoop.h>
#include <iiSDKCore/Runtime/LiveStream.h>
#include <iiSDKCore/Runtime/Thread.h>
#include <iostream>
#include <future>
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
void realtime_precedes_normal_backlog() {
  EventLoop loop(4097);
  int ordinary=0,realtime=0;
  for (int i=0;i<4096;++i)
    check(bool(loop.post([&]{++ordinary;})),"admit ordinary backlog");
  RealtimeSchedule plan{10s,1s,100ms,10ms,Clock::now()};
  check(bool(loop.realtime(plan,[&](const auto&) {++realtime;return SliceResult::stop;})),
        "admit realtime work behind ordinary backlog");
  check(loop.poll({1,1s})==1 && realtime==1 && ordinary==0,
        "a ready realtime reservation precedes a large ordinary backlog");
  loop.request_stop();
  check(loop.stats().pending==0,"backlog cancellation returns every slot");
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
void live_demand() {
  EventLoop loop(4);
  LiveStream<int> stream(4, 1h);
  std::vector<int> received;
  RealtimeSchedule plan{10ms, 10ms, 2ms, 1ms, Clock::now()};
  const auto playback = stream.start(loop, plan, [&](auto frame) {
    received.push_back(frame.value);
  });
  check(bool(playback), "demand stream reservation admitted");
  check(loop.poll({10, 50ms}) == 0 && !loop.next_deadline() &&
            loop.stats().slices == 0,
        "empty stream does not schedule recurring callbacks");
  check(stream.push(Clock::now(), 1), "push wakes parked playback");
  check(loop.poll({10, 50ms}) == 1 && received == std::vector<int>{1},
        "data is delivered when pushed");
  check(!loop.next_deadline(), "drained stream returns to idle");
  const auto future = Clock::now() + 1h;
  check(stream.push(future, 2), "accept future data");
  check(loop.next_deadline() && *loop.next_deadline() >= future &&
            loop.poll({10, 50ms}) == 0,
        "future data arms its presentation time, not periodic polling");
  check(bool(stream.close()) && !playback.token.pending(),
        "parked reservation closes and releases its slot");
}
void signal_delivery() {
  EventLoop loop(4);
  auto sender=loop.sender();
  int calls=0;
  TaskToken token;
  auto subscription=loop.on_signal([&] {
    if (++calls==1) {
      check(sender.signal(token,Clock::now()),"signal during callback");
      check(sender.signal(token,Clock::now()),"coalesce concurrent signals");
    }
  });
  token=subscription.token;
  check(subscription && !loop.next_deadline() && loop.poll()==0,
        "signal callback remains dormant before notification");
  check(sender.signal(token,Clock::now()) && sender.signal(token,Clock::now()),
        "signal callback is triggered by state change");
  check(loop.poll({10,50ms})==2 && calls==2 && !loop.next_deadline(),
        "signals coalesce and a signal during delivery is preserved");
  check(token.cancel() && !sender.signal(token,Clock::now()),
        "cancel retires the reusable signal slot");
  auto failing=loop.on_signal([] { throw std::runtime_error("signal failure"); });
  check(failing && sender.signal(failing.token,Clock::now()),"throwing signal admitted");
  bool raised=false;
  try { loop.poll(); } catch(const std::runtime_error&) { raised=true; }
  check(raised && !failing.token.pending(),"throwing signal retires its slot and propagates");
  int extreme_calls=0;
  const auto extreme=loop.on_signal([&] { ++extreme_calls; });
  check(extreme && sender.signal(extreme.token,Clock::time_point::max()) &&
        loop.next_deadline()==Clock::time_point::max() && loop.poll()==0,
        "a maximum-time wake remains distinct from a parked binding");
  check(sender.signal(extreme.token,Clock::time_point::min()) && loop.poll()==1 &&
        extreme_calls==1 && extreme.token.cancel(),"an earlier extreme wake replaces the maximum deadline");
}
void tiny_pump_budget() {
  EventLoop loop(2);
  int calls=0;
  check(bool(loop.post([&] { ++calls; })),"tiny-budget work admitted");
  check(loop.poll({1,1ns})==1 && calls==1,
        "positive pump budget makes progress instead of spinning");
  check(bool(loop.post([&] { ++calls; })) && loop.poll({1,0ns})==0,
        "zero time budget still admits no callback");
  loop.request_stop();
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
void live_concurrent_push() {
  EventLoop loop(4);
  LiveStream<int> stream(4, 1h);
  std::latch entered(1), release(1);
  std::promise<void> delivered;
  auto done=delivered.get_future();
  std::atomic<int> count=0;
  check(bool(stream.start(loop,RealtimeSchedule{},[&](auto) {
    if (++count==1) { entered.count_down(); release.wait(); }
    else delivered.set_value();
  })),"start demand playback for concurrent producer");
  check(stream.push(Clock::now(),1),"queue first frame");
  Thread runner;
  check(bool(runner.start(loop)),"start demand playback driver");
  entered.wait();
  const bool accepted=stream.push(Clock::now(),2);
  release.count_down();
  const bool received=done.wait_for(2s)==std::future_status::ready;
  loop.request_stop();
  check(bool(runner.join()),"join demand playback driver");
  check(accepted && received && count==2 && bool(stream.close()),
        "a push during delivery cannot lose its wakeup");
}
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
    realtime_precedes_normal_backlog();
    live_stream();
    live_demand();
    signal_delivery();
    tiny_pump_budget();
    continuations_and_removal();
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    live_concurrent_push();
    active_live_removal();
    native_driver();
#endif
    std::cout << "realtime scheduling and timestamped playout passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
