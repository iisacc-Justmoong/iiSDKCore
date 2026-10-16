#include "FrameLoop.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ii::runtime {
namespace {
bool fixed_phase(FramePhase phase) {
  return phase == FramePhase::before_fixed || phase == FramePhase::fixed_update ||
         phase == FramePhase::after_fixed;
}
Clock::duration add_time(Clock::duration left, Clock::duration right) {
  return core::Time::add(Clock::time_point(left), right).time_since_epoch();
}
Clock::duration scale_time(Clock::duration value, double scale) {
  const auto ticks = static_cast<long double>(value.count()) * scale;
  if (ticks >= static_cast<long double>(Clock::duration::max().count()))
    return Clock::duration::max();
  return Clock::duration(static_cast<Clock::duration::rep>(ticks));
}
} // namespace

struct FrameLoop::Impl final {
  struct Entry final {
    graph::NodeId id;
    Tick tick;
    TickOptions options;
    std::optional<Clock::duration> last_tick;
    bool executing = false, retiring = false;
  };
  struct Scheduled final {
    graph::NodeId id;
    TickOptions options;
  };

  EventLoop &events;
  const FrameLoopOptions options;
  const std::thread::id owner = std::this_thread::get_id();
  graph::DependencyGraph graph;
  std::vector<Entry> entries;
  std::vector<graph::NodeId> order;
  std::vector<Scheduled> schedule;
  FrameStats statistics;
  std::optional<Clock::time_point> previous;
  Clock::duration elapsed{}, unscaled_elapsed{}, fixed_elapsed{}, accumulator{};
  std::uint64_t fixed_tick = 0;
  bool paused = false, inside_frame = false, driving = false, dirty = true;
  bool closing = false;
  std::size_t retiring_depth = 0;
  double time_scale = 1;

  Impl(EventLoop &loop, FrameLoopOptions configuration)
      : events(loop), options(configuration), graph(options.capacity),
        entries(options.capacity) {
    if (options.fixed_step <= Clock::duration::zero() ||
        options.max_delta <= Clock::duration::zero() || !options.max_fixed_steps ||
        options.frame_interval <= Clock::duration::zero() ||
        options.frame_budget <= Clock::duration::zero())
      throw std::invalid_argument("FrameLoop requires positive step, limits, interval and budget");
    schedule.reserve(options.capacity);
    statistics.capacity = options.capacity;
  }
  void check_owner() const {
    if (std::this_thread::get_id() != owner)
      throw std::logic_error("FrameLoop operations require its constructing host thread");
  }
  void check_pump() const {
    check_owner();
    if (inside_frame || closing || retiring_depth)
      throw std::logic_error("FrameLoop cannot recursively advance or advance during removal");
  }
  bool valid(graph::NodeId id) const {
    return graph.contains(id) && !entries[id.slot].retiring;
  }
  void retire(graph::NodeId id, Tick &capture) {
    auto &entry = entries[id.slot];
    entry.retiring = true;
    // Capture destructors may reenter registration. Keep the slot occupied
    // until they finish, and never expose it as a live registration again.
    ++retiring_depth;
    capture = nullptr;
    (void)graph.remove(id);
    entry.id = {};
    entry.last_tick.reset();
    entry.executing = false;
    --statistics.active_ticks;
    --retiring_depth;
    dirty = true;
  }
  void snapshot() {
    if (dirty) {
      order = std::move(*graph.order());
      dirty = false;
    }
    schedule.clear();
    for (auto id : order)
      if (valid(id)) schedule.push_back({id, entries[id.slot].options});
  }
  void phase(FramePhase phase, FrameContext context, FrameReport &report) {
    context.phase = phase;
    context.fixed_tick = fixed_tick;
    context.fixed_elapsed = fixed_elapsed;
    for (const auto &item : schedule) {
      if (events.state() != LoopState::ready) break;
      if (item.options.phase != phase || !item.options.enabled || !valid(item.id) ||
          (context.paused && item.options.clock == TickClock::scaled))
        continue;
      auto &entry = entries[item.id.slot];
      const auto clock = fixed_phase(phase) ? fixed_elapsed :
          item.options.clock == TickClock::unscaled ? unscaled_elapsed : elapsed;
      auto time = context;
      if (item.options.clock == TickClock::unscaled) time.delta = context.unscaled_delta;
      if (entry.last_tick && item.options.interval > Clock::duration::zero()) {
        const auto delta = clock - *entry.last_tick;
        if (delta < item.options.interval) continue;
        time.delta = delta;
      }
      entry.last_tick = clock;
      entry.executing = true;
      Tick tick;
      tick.swap(entry.tick);
      std::exception_ptr failure;
      try { tick(time); } catch (...) { failure = std::current_exception(); }
      ++report.callbacks;
      ++statistics.callbacks;
      if (failure || entry.retiring) {
        retire(item.id, tick);
      } else {
        entry.executing = false;
        entry.tick.swap(tick);
      }
      if (failure) {
        ++statistics.failed_ticks;
        std::rethrow_exception(failure);
      }
    }
  }
  void discard_backlog(FrameReport &report) {
    const auto steps = accumulator / options.fixed_step;
    if (steps > 0) {
      report.dropped_fixed_steps += static_cast<std::uint64_t>(steps);
      accumulator %= options.fixed_step;
    }
    report.interpolation = static_cast<double>(accumulator.count()) /
                           options.fixed_step.count();
  }
  void finish(FrameReport &report, Clock::time_point started, bool failed) {
    discard_backlog(report);
    statistics.dropped_fixed_steps += report.dropped_fixed_steps;
    if (failed) ++statistics.failed_frames;
    report.work_time = core::Time::elapsed(started, Clock::now());
    report.over_budget = report.work_time > options.frame_budget;
    if (report.over_budget) ++statistics.overruns;
    statistics.max_work_time = std::max(statistics.max_work_time, report.work_time);
    inside_frame = false;
  }
};

FrameLoop::FrameLoop(EventLoop &events, FrameLoopOptions options)
    : impl_(std::make_unique<Impl>(events, options)) {}
FrameLoop::~FrameLoop() {
  impl_->closing = true;
  for (auto &entry : impl_->entries)
    if (impl_->valid(entry.id)) {
      Tick tick;
      tick.swap(entry.tick);
      impl_->retire(entry.id, tick);
    }
}

core::Result<TickToken> FrameLoop::add(Tick tick, TickOptions options) {
  impl_->check_owner();
  if (impl_->closing || impl_->events.state() == LoopState::stopped)
    return core::fail(core::ErrorCode::closed, "FrameLoop registration is closed");
  if (!tick || options.phase < FramePhase::begin || options.phase > FramePhase::end ||
      (options.clock != TickClock::scaled && options.clock != TickClock::unscaled) ||
      options.interval < Clock::duration::zero() ||
      (fixed_phase(options.phase) && options.clock == TickClock::unscaled))
    return core::fail(core::ErrorCode::invalid_argument, "Invalid frame tick options or callback");
  auto node = impl_->graph.add();
  if (!node) return std::unexpected(node.error());
  auto &entry = impl_->entries[node->slot];
  entry.id = *node;
  entry.options = options;
  entry.last_tick.reset();
  entry.executing = entry.retiring = false;
  entry.tick.swap(tick);
  ++impl_->statistics.active_ticks;
  impl_->dirty = true;
  return TickToken(*node);
}
core::Result<void> FrameLoop::remove(TickToken token) {
  impl_->check_owner();
  if (!impl_->valid(token.id_))
    return core::fail(core::ErrorCode::not_found, "Invalid or retired frame tick");
  auto &entry = impl_->entries[token.id_.slot];
  entry.retiring = true;
  if (!entry.executing) {
    Tick tick;
    tick.swap(entry.tick);
    impl_->retire(token.id_, tick);
  }
  return {};
}
bool FrameLoop::contains(TickToken token) const {
  impl_->check_owner();
  return impl_->valid(token.id_);
}
core::Result<void> FrameLoop::set_enabled(TickToken token, bool enabled) {
  impl_->check_owner();
  if (!impl_->valid(token.id_))
    return core::fail(core::ErrorCode::not_found, "Invalid or retired frame tick");
  impl_->entries[token.id_.slot].options.enabled = enabled;
  return {};
}
core::Result<void> FrameLoop::add_dependency(TickToken dependent, TickToken prerequisite) {
  impl_->check_owner();
  if (!impl_->valid(dependent.id_) || !impl_->valid(prerequisite.id_))
    return core::fail(core::ErrorCode::not_found, "Invalid frame dependency endpoint");
  if (impl_->entries[prerequisite.id_.slot].options.phase >
      impl_->entries[dependent.id_.slot].options.phase)
    return core::fail(core::ErrorCode::invalid_argument, "Frame dependency cannot reverse phase order");
  auto result = impl_->graph.connect(prerequisite.id_, dependent.id_);
  if (result) impl_->dirty = true;
  return result;
}
core::Result<void> FrameLoop::remove_dependency(TickToken dependent, TickToken prerequisite) {
  impl_->check_owner();
  if (!impl_->valid(dependent.id_) || !impl_->valid(prerequisite.id_))
    return core::fail(core::ErrorCode::not_found, "Invalid frame dependency endpoint");
  auto result = impl_->graph.disconnect(prerequisite.id_, dependent.id_);
  if (result) impl_->dirty = true;
  return result;
}
void FrameLoop::set_paused(bool paused) {
  impl_->check_owner();
  impl_->paused = paused;
}
core::Result<void> FrameLoop::set_time_scale(double scale) {
  impl_->check_owner();
  if (!std::isfinite(scale) || scale < 0)
    return core::fail(core::ErrorCode::invalid_argument, "Frame time scale must be finite and nonnegative");
  impl_->time_scale = scale;
  return {};
}
void FrameLoop::reset_clock(Clock::time_point now) {
  impl_->check_pump();
  impl_->previous = now;
  impl_->accumulator = {};
}

FrameReport FrameLoop::step(Clock::time_point now) {
  impl_->check_pump();
  FrameReport report;
  if (impl_->events.state() != LoopState::ready) return report;
  if (impl_->previous && now < *impl_->previous)
    throw std::invalid_argument("FrameLoop host timestamps must be monotonic; use reset_clock to rebase");
  const auto started = Clock::now();
  impl_->snapshot();
  const auto raw = impl_->previous ? core::Time::elapsed(*impl_->previous, now) : Clock::duration{};
  impl_->previous = now;
  const auto paused = impl_->paused || impl_->time_scale == 0;
  const auto scaled = paused ? Clock::duration{} : scale_time(raw, impl_->time_scale);
  const auto delta = std::min(scaled, impl_->options.max_delta);
  report.clamped_delta = scaled - delta;
  if (report.clamped_delta > Clock::duration::zero()) ++impl_->statistics.clamped_frames;
  impl_->elapsed = add_time(impl_->elapsed, delta);
  impl_->unscaled_elapsed = add_time(impl_->unscaled_elapsed, raw);
  impl_->accumulator = add_time(impl_->accumulator, delta);
  const auto pending = static_cast<std::uint64_t>(impl_->accumulator / impl_->options.fixed_step);
  const auto steps = std::min<std::uint64_t>(pending, impl_->options.max_fixed_steps);
  report.dropped_fixed_steps = pending - steps;
  impl_->accumulator -= impl_->options.fixed_step *
      static_cast<Clock::duration::rep>(report.dropped_fixed_steps);
  FrameContext context{.frame = ++impl_->statistics.frames, .now = now, .delta = delta,
      .unscaled_delta = raw, .elapsed = impl_->elapsed, .unscaled_elapsed = impl_->unscaled_elapsed,
      .interpolation = static_cast<double>((impl_->accumulator % impl_->options.fixed_step).count()) /
          impl_->options.fixed_step.count(), .paused = paused};
  report.frame = context.frame;
  report.advanced = true;
  impl_->inside_frame = true;
  try {
    impl_->phase(FramePhase::begin, context, report);
    impl_->phase(FramePhase::input, context, report);
    for (std::uint64_t i = 0; i < steps && !paused &&
         impl_->events.state() == LoopState::ready; ++i) {
      impl_->accumulator -= impl_->options.fixed_step;
      impl_->fixed_elapsed = add_time(impl_->fixed_elapsed, impl_->options.fixed_step);
      ++impl_->fixed_tick;
      ++impl_->statistics.fixed_steps;
      ++report.fixed_steps;
      auto fixed = context;
      fixed.delta = impl_->options.fixed_step;
      impl_->phase(FramePhase::before_fixed, fixed, report);
      impl_->phase(FramePhase::fixed_update, fixed, report);
      impl_->phase(FramePhase::after_fixed, fixed, report);
    }
    for (auto phase : {FramePhase::update, FramePhase::late_update,
             FramePhase::prepare_present, FramePhase::present, FramePhase::end})
      impl_->phase(phase, context, report);
  } catch (...) {
    impl_->finish(report, started, true);
    throw;
  }
  impl_->finish(report, started, false);
  return report;
}
FrameReport FrameLoop::poll(PumpBudget budget) {
  impl_->check_pump();
  impl_->events.poll(budget);
  return step(Clock::now());
}
void FrameLoop::run(PumpBudget budget) {
  impl_->check_pump();
  if (impl_->driving) throw std::logic_error("FrameLoop native driver is already active");
  const auto timer = impl_->events.every(impl_->options.frame_interval,
      [this] { step(Clock::now()); });
  if (!timer) throw std::runtime_error("FrameLoop cannot reserve an EventLoop timer slot");
  impl_->driving = true;
  try { impl_->events.run(budget); }
  catch (...) {
    (void)timer.token.cancel();
    impl_->driving = false;
    throw;
  }
  (void)timer.token.cancel();
  impl_->driving = false;
}
FrameStats FrameLoop::stats() const {
  impl_->check_owner();
  return impl_->statistics;
}
EventLoop &FrameLoop::events() const noexcept { return impl_->events; }

} // namespace ii::runtime
