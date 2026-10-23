#include <iiSDKCore/Runtime/FrameLoop.h>
#include <iiSDKCore/Runtime/Thread.h>

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace ii::runtime;
using namespace std::chrono_literals;

namespace {
void check(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
template <class T> T take(ii::core::Result<T> result) {
  check(bool(result), "frame operation failed");
  return std::move(*result);
}
constexpr auto origin = Clock::time_point(1s);

void phases_and_fixed_time() {
  EventLoop events;
  FrameLoop frames(events, {.fixed_step = 10ms});
  std::vector<FramePhase> order;
  std::vector<FrameContext> fixed;
  constexpr std::array phases{FramePhase::begin, FramePhase::input,
      FramePhase::before_fixed, FramePhase::fixed_update,
      FramePhase::after_fixed, FramePhase::update, FramePhase::late_update,
      FramePhase::prepare_present, FramePhase::present, FramePhase::end};
  for (auto phase : phases)
    take(frames.add([&, phase](const FrameContext &time) {
      check(time.phase == phase, "phase context matches callback");
      order.push_back(phase);
      if (phase == FramePhase::fixed_update) fixed.push_back(time);
    }, {.phase = phase}));
  check(frames.step(origin).fixed_steps == 0, "first frame establishes clock");
  order.clear();
  auto report = frames.step(origin + 25ms);
  std::vector<FramePhase> expected{FramePhase::begin, FramePhase::input,
      FramePhase::before_fixed, FramePhase::fixed_update, FramePhase::after_fixed,
      FramePhase::before_fixed, FramePhase::fixed_update, FramePhase::after_fixed,
      FramePhase::update, FramePhase::late_update, FramePhase::prepare_present,
      FramePhase::present, FramePhase::end};
  check(order == expected, "fixed triplets finish before variable stages");
  check(report.callbacks == 13 && report.fixed_steps == 2 &&
      report.dropped_fixed_steps == 0 && std::abs(report.interpolation - .5) < 1e-6,
      "fixed accumulator and interpolation");
  check(fixed.size() == 2 && fixed[0].delta == 10ms && fixed[1].delta == 10ms &&
      fixed[0].fixed_elapsed == 10ms && fixed[1].fixed_elapsed == 20ms &&
      fixed[1].elapsed == 25ms && fixed[1].unscaled_delta == 25ms,
      "fixed and wall clocks are explicit");
  check(frames.step(origin + 30ms).fixed_steps == 1, "fraction is carried forward");
}

void ordering_and_identity() {
  EventLoop events;
  FrameLoop frames(events, {.capacity = 4});
  FrameLoop other(events);
  std::vector<int> order;
  auto a = take(frames.add([&](const auto &) { order.push_back(1); }));
  auto b = take(frames.add([&](const auto &) { order.push_back(2); }));
  auto c = take(frames.add([&](const auto &) { order.push_back(3); }));
  check(bool(frames.add_dependency(a, c)) && bool(frames.add_dependency(b, a)),
      "tick prerequisites");
  auto cycle = frames.add_dependency(c, b);
  check(!cycle && cycle.error().code == ii::core::ErrorCode::cycle,
      "cycles are rejected");
  frames.step(origin);
  check(order == std::vector<int>({3, 1, 2}), "topological execution");
  auto early = take(frames.add([](const auto &) {}, {.phase = FramePhase::input}));
  auto backward = frames.add_dependency(early, a);
  check(!backward && backward.error().code == ii::core::ErrorCode::invalid_argument,
      "a dependency cannot reverse phases");
  check(!other.remove(a), "foreign token rejected");
  check(bool(frames.remove(a)), "remove tick");
  auto replacement = take(frames.add([](const auto &) {}));
  check(replacement != a && !frames.contains(a) && !frames.remove(a),
      "generation prevents stale cancellation");
  check(!frames.add([](const auto &) {}) && frames.stats().active_ticks == 4,
      "bounded tick slots");
  auto removed_edge = frames.remove_dependency(b, a);
  check(!removed_edge, "removal also removes incident prerequisites");
}

void pause_scale_and_intervals() {
  EventLoop events;
  FrameLoop frames(events, {.fixed_step = 10ms});
  std::vector<FrameContext> scaled, real;
  take(frames.add([&](const auto &time) { scaled.push_back(time); }));
  take(frames.add([&](const auto &time) { real.push_back(time); },
      {.clock = TickClock::unscaled, .interval = 20ms}));
  frames.step(origin);
  frames.set_paused(true);
  auto paused = frames.step(origin + 30ms);
  check(paused.fixed_steps == 0 && scaled.size() == 1 && real.size() == 2 &&
      real.back().paused && real.back().delta == 30ms && real.back().elapsed == 0ms,
      "unscaled UI ticks survive simulation pause");
  frames.set_paused(false);
  check(bool(frames.set_time_scale(.5)), "set simulation rate");
  check(frames.step(origin + 50ms).fixed_steps == 1 && scaled.back().delta == 10ms &&
      scaled.back().elapsed == 10ms && real.back().unscaled_elapsed == 50ms,
      "scaled simulation and real interval are independent");
  check(bool(frames.set_time_scale(0)), "zero scale pauses simulation");
  frames.step(origin + 70ms);
  check(scaled.size() == 2 && real.back().paused, "zero scale has pause semantics");
  for (double rate : {-1., std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::quiet_NaN()})
    check(!frames.set_time_scale(rate), "invalid simulation rates rejected");

  EventLoop interval_events;
  FrameLoop intervals(interval_events);
  std::vector<Clock::duration> deltas;
  auto token = take(intervals.add([&](const auto &time) { deltas.push_back(time.delta); },
      {.interval = 25ms}));
  intervals.step(origin);
  intervals.step(origin + 10ms);
  intervals.step(origin + 60ms);
  check(deltas == std::vector<Clock::duration>({0ms, 60ms}),
      "interval ticks coalesce elapsed time rather than burst");
  check(bool(intervals.set_enabled(token, false)), "disable tick");
  intervals.step(origin + 100ms);
  check(bool(intervals.set_enabled(token, true)), "enable tick");
  intervals.step(origin + 110ms);
  check(deltas.back() == 50ms, "reenabled interval observes time since last delivery");
}

void host_cadence_and_display_changes() {
  // Deterministic host timestamps, not measured display refresh or scanout.
  // No nominal 16 ms timer is involved in the externally driven path.
  for (const std::int64_t hz : {60, 120, 144}) {
    EventLoop events;
    FrameLoop frames(events, {.fixed_step = 10ms});
    std::uint64_t updates = 0, presents = 0, fixed = 0;
    Clock::duration elapsed{};
    take(frames.add([&](const FrameContext &time) {
      check(time.frame == ++updates, "variable update follows every host frame exactly once");
      elapsed = time.unscaled_elapsed;
    }, {.clock = TickClock::unscaled}));
    take(frames.add([&](const FrameContext &time) {
      check(time.delta == 10ms, "fixed simulation step is independent of display cadence");
      ++fixed;
    }, {.phase = FramePhase::fixed_update}));
    take(frames.add([&](const auto &) { ++presents; },
        {.phase = FramePhase::present, .clock = TickClock::unscaled}));
    frames.step(origin);
    for (std::int64_t i = 1; i <= hz; ++i)
      frames.step(origin + std::chrono::duration_cast<Clock::duration>(
          std::chrono::nanoseconds(i * 1'000'000'000 / hz)));
    check(updates == static_cast<std::uint64_t>(hz + 1) && presents == updates &&
        fixed == 100 && elapsed == 1s && frames.stats().dropped_fixed_steps == 0,
        "60/120/144 Hz hosts share one variable loop and a 100 Hz simulation");
    check(events.stats().pending == 0, "external frames reserve no periodic event timer");
  }

  EventLoop events;
  FrameLoop frames(events, {.fixed_step = 10ms});
  std::uint64_t updates = 0, fixed = 0, throttled = 0;
  std::uint64_t previous_throttled_frame = 0;
  take(frames.add([&](const FrameContext &time) {
    check(time.frame == ++updates, "refresh changes neither skip nor duplicate host frames");
  }, {.clock = TickClock::unscaled}));
  take(frames.add([&](const auto &) { ++fixed; }, {.phase = FramePhase::fixed_update}));
  take(frames.add([&](const FrameContext &time) {
    check(time.frame > previous_throttled_frame && (!throttled || time.delta >= 50ms),
        "interval ticks remain frame-bound and do not replay missed intervals");
    previous_throttled_frame = time.frame;
    ++throttled;
  }, {.clock = TickClock::unscaled, .interval = 50ms}));
  frames.step(origin);
  auto segment = origin;
  for (const std::int64_t hz : {60, 120, 144}) {
    for (std::int64_t i = 1; i <= hz; ++i)
      frames.step(segment + std::chrono::duration_cast<Clock::duration>(
          std::chrono::nanoseconds(i * 1'000'000'000 / hz)));
    segment += 1s;
  }
  check(updates == 325 && fixed == 300 && throttled > 1 && throttled <= 61 &&
      frames.stats().clamped_frames == 0 && frames.stats().dropped_fixed_steps == 0 &&
      events.stats().pending == 0,
      "60 to 120 to 144 Hz changes preserve simulation time without a second driver");
  std::cout << "FRAME_CADENCE_OK hosts=60,120,144 fixed_hz=100 switched_frames="
            << updates << " timers=" << events.stats().pending << '\n';
}

void bounded_catchup_and_extremes() {
  EventLoop events;
  FrameLoop frames(events, {.fixed_step = 10ms, .max_delta = 100ms,
      .max_fixed_steps = 3});
  frames.step(origin);
  auto report = frames.step(origin + 1005ms);
  check(report.fixed_steps == 3 && report.dropped_fixed_steps == 7 &&
      report.clamped_delta == 905ms && report.interpolation == 0.,
      "long stalls cap delta and discard bounded fixed backlog");
  check(frames.stats().dropped_fixed_steps == 7 && frames.stats().clamped_frames == 1,
      "lost simulation work is observable");
  check(frames.step(origin + 1010ms).interpolation == .5,
      "discarded backlog is not replayed later");
  bool backwards = false;
  try { frames.step(origin); } catch (const std::invalid_argument &) { backwards = true; }
  check(backwards, "backwards host timestamps are rejected");
  check(bool(frames.set_time_scale(std::numeric_limits<double>::max())),
      "finite large rates are accepted with saturating arithmetic");
  check(frames.step(Clock::time_point::max()).fixed_steps == 3,
      "maximum timestamp and scaled delta do not overflow");
  frames.reset_clock(origin);
  check(frames.step(origin + 5ms).fixed_steps == 3, "host clock can explicitly rebase");
}

void mutation_and_failure_cleanup() {
  EventLoop events;
  FrameLoop frames(events, {.capacity = 3, .fixed_step = 10ms});
  int old_calls = 0, added_calls = 0;
  TickToken victim, replacement;
  auto mutator = take(frames.add([&](const auto &) {
    check(bool(frames.remove(victim)), "remove not-yet-called tick immediately");
    replacement = take(frames.add([&](const auto &) { ++added_calls; }));
  }, {.phase = FramePhase::input}));
  victim = take(frames.add([&](const auto &) { ++old_calls; }));
  frames.step(origin);
  check(old_calls == 0 && added_calls == 0, "registration starts at next frame");
  check(bool(frames.remove(mutator)), "remove mutator");
  frames.step(origin + 10ms);
  check(added_calls == 1 && frames.contains(replacement), "replacement is delivered next frame");

  std::weak_ptr<int> capture;
  TickToken failing;
  {
    auto owned = std::make_shared<int>(42);
    capture = owned;
    failing = take(frames.add([owned](const auto &) {
      throw std::runtime_error("expected frame failure");
    }));
  }
  check(bool(frames.add_dependency(failing, replacement)), "failure follows independent surviving tick");
  bool caught = false;
  try { frames.step(origin + 20ms); } catch (const std::runtime_error &) { caught = true; }
  check(caught && capture.expired() && !frames.contains(failing) &&
      frames.stats().failed_ticks == 1 && frames.stats().failed_frames == 1,
      "a failing tick releases captures and propagates after retirement");
  frames.step(origin + 30ms);
  check(added_calls == 3, "other registrations survive a failed frame");

  EventLoop self_events;
  FrameLoop self(self_events, {.capacity = 1});
  TickToken self_token;
  bool full_during_callback = false;
  self_token = take(self.add([&](const auto &) {
    check(bool(self.remove(self_token)), "self removal");
    auto denied = self.add([](const auto &) {});
    full_during_callback = !denied && denied.error().code == ii::core::ErrorCode::full;
  }));
  self.step(origin);
  check(full_during_callback && self.stats().active_ticks == 0 &&
      bool(self.add([](const auto &) {})), "executing capture retires before capacity is returned");
}

void guards_and_frame_boundary() {
  EventLoop events;
  FrameLoop frames(events);
  bool recursive = false;
  int simulation = 0, ui = 0;
  take(frames.add([&](const auto &) {
    try { frames.step(origin); } catch (const std::logic_error &) { recursive = true; }
    frames.set_paused(true);
  }, {.phase = FramePhase::input, .clock = TickClock::unscaled}));
  take(frames.add([&](const auto &) { ++simulation; }));
  take(frames.add([&](const auto &) { ++ui; }, {.clock = TickClock::unscaled}));
  frames.step(origin);
  frames.step(origin + 10ms);
  check(recursive && simulation == 1 && ui == 2, "clock controls take effect on the next frame");
  events.suspend();
  check(!frames.step(origin + 30ms).advanced && ui == 2, "suspended event loop suppresses frames");
  events.resume();
  check(frames.step(origin + 40ms).advanced, "resumed host advances frames");
  events.request_stop();
  check(!frames.step(origin + 50ms).advanced, "stopped event loop cannot advance frames");
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
  Thread worker;
  bool wrong_thread = false;
  check(bool(worker.start([&](ThreadContext &) {
    try { frames.step(origin + 50ms); }
    catch (const std::logic_error &) { wrong_thread = true; }
  })) && bool(worker.join()) && wrong_thread, "host thread affinity is enforced");
#endif
}

void capture_retirement_and_options_snapshot() {
  EventLoop events;
  FrameLoop frames(events, {.capacity = 1});
  bool observed_full = false, reentry_rejected = false;
  struct OnDestroy final {
    std::function<void()> action;
    ~OnDestroy() { action(); }
  };
  auto capture = std::make_shared<OnDestroy>([&] {
    const auto added = frames.add([](const auto &) {});
    observed_full = !added && added.error().code == ii::core::ErrorCode::full;
    try { frames.step(origin); } catch (const std::logic_error &) { reentry_rejected = true; }
  });
  auto token = take(frames.add([capture](const auto &) {}));
  capture.reset();
  check(bool(frames.remove(token)) && observed_full && reentry_rejected &&
      frames.stats().active_ticks == 0, "capture destruction precedes slot return and rejects pumping");

  EventLoop snapshot_events;
  FrameLoop snapshot(snapshot_events);
  int updates = 0;
  TickToken update;
  take(snapshot.add([&](const auto &) {
    check(bool(snapshot.set_enabled(update, false)), "disable later tick from input");
  }, {.phase = FramePhase::input}));
  update = take(snapshot.add([&](const auto &) { ++updates; }));
  snapshot.step(origin);
  snapshot.step(origin + 5ms);
  check(updates == 1, "enable options remain fixed within a frame");

  EventLoop budget_events;
  FrameLoop budget(budget_events, {.frame_budget = Clock::duration(1)});
  take(budget.add([](const auto &) {}));
  const auto report = budget.step(origin);
  check(report.over_budget && budget.stats().overruns == 1 &&
      budget.stats().max_work_time == report.work_time, "cooperative frame overruns are observable");
}

void event_integration_and_native_driver() {
  EventLoop events;
  FrameLoop frames(events, {.frame_interval = 1ms});
  bool event_done = false;
  take(frames.add([&](const auto &) { check(event_done, "events finish before frame input"); }));
  check(bool(events.post([&] { event_done = true; })), "post event work");
  check(frames.poll().advanced, "nonblocking host pumps events and frame together");
#if !defined(__EMSCRIPTEN__)
  int count = 0;
  take(frames.add([&](const auto &) { if (++count == 3) events.request_stop(); },
      {.phase = FramePhase::end}));
  frames.run();
  check(count == 3 && events.stats().pending == 0, "native driver stops and retires its timer");
  EventLoop failed_events;
  FrameLoop failed(failed_events, {.frame_interval = 1ms});
  take(failed.add([](const auto &) { throw std::runtime_error("native tick failure"); }));
  bool caught = false;
  try { failed.run(); } catch (const std::runtime_error &) { caught = true; }
  check(caught && failed_events.stats().pending == 0, "native timer retires after callback failure");
  EventLoop invalid_events(1);
  FrameLoop invalid(invalid_events);
  bool invalid_budget = false;
  try { invalid.run({0, 1ms}); } catch (const std::invalid_argument &) { invalid_budget = true; }
  check(invalid_budget && invalid_events.stats().pending == 0,
      "invalid driver budget cannot leak its reserved timer");
  auto occupied = invalid_events.post([] {});
  bool full = false;
  try { invalid.run(); } catch (const std::runtime_error &) { full = true; }
  check(bool(occupied) && full && invalid_events.stats().pending == 1,
      "frame timer reservation fails without removing other owners' work");
#endif
}

void validation() {
  EventLoop events;
  for (const auto &options : {FrameLoopOptions{.capacity = 0},
      FrameLoopOptions{.fixed_step = 0ms}, FrameLoopOptions{.max_delta = 0ms},
      FrameLoopOptions{.max_fixed_steps = 0}, FrameLoopOptions{.frame_interval = 0ms},
      FrameLoopOptions{.frame_budget = 0ms}}) {
    bool caught = false;
    try { FrameLoop frames(events, options); } catch (const std::invalid_argument &) { caught = true; }
    check(caught, "invalid frame options rejected");
  }
  FrameLoop frames(events);
  check(!frames.add({}) && !frames.add([](const auto &) {}, {.interval = -1ms}) &&
      !frames.add([](const auto &) {}, {.phase = static_cast<FramePhase>(100)}) &&
      !frames.add([](const auto &) {}, {.clock = static_cast<TickClock>(100)}) &&
      !frames.add([](const auto &) {}, {.phase = FramePhase::fixed_update,
          .clock = TickClock::unscaled}), "invalid callback policies rejected");
}
} // namespace

int main() {
  try {
    phases_and_fixed_time();
    ordering_and_identity();
    pause_scale_and_intervals();
    host_cadence_and_display_changes();
    bounded_catchup_and_extremes();
    mutation_and_failure_cleanup();
    guards_and_frame_boundary();
    capture_retirement_and_options_snapshot();
    event_integration_and_native_driver();
    validation();
    std::cout << "frame loop contracts passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
