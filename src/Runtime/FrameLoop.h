#pragma once

#include "EventLoop.h"
#include "../Core/Result/Result.h"
#include "../Graph/DependencyGraph.h"

namespace ii::runtime {

enum class FramePhase {
  begin, input, before_fixed, fixed_update, after_fixed, update, late_update,
  prepare_present, present, end
};
enum class TickClock { scaled, unscaled };

class TickToken final {
public:
  TickToken() = default;
  friend bool operator==(const TickToken &, const TickToken &) = default;
private:
  friend class FrameLoop;
  explicit TickToken(graph::NodeId id) : id_(id) {}
  graph::NodeId id_;
};

struct TickOptions final {
  FramePhase phase = FramePhase::update;
  TickClock clock = TickClock::scaled;
  Clock::duration interval{}; // Minimum time between deliveries; no burst.
  bool enabled = true;
};

struct FrameContext final {
  FramePhase phase = FramePhase::begin;
  std::uint64_t frame = 0, fixed_tick = 0;
  Clock::time_point now{};
  Clock::duration delta{}, unscaled_delta{};
  Clock::duration elapsed{}, unscaled_elapsed{}, fixed_elapsed{};
  double interpolation = 0;
  bool paused = false;
};
using Tick = std::function<void(const FrameContext &)>;

struct FrameLoopOptions final {
  std::size_t capacity = 128;
  Clock::duration fixed_step =
      std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(16'666'667));
  Clock::duration max_delta = std::chrono::milliseconds(250);
  std::size_t max_fixed_steps = 4;
  Clock::duration frame_interval = std::chrono::milliseconds(16);
  // Measured cooperative budget. A callback/phase is never forcibly preempted.
  Clock::duration frame_budget = std::chrono::milliseconds(16);
};

struct FrameReport final {
  bool advanced = false;
  std::uint64_t frame = 0;
  std::size_t callbacks = 0, fixed_steps = 0;
  std::uint64_t dropped_fixed_steps = 0;
  Clock::duration clamped_delta{}, work_time{};
  double interpolation = 0;
  bool over_budget = false;
};

struct FrameStats final {
  std::size_t capacity = 0, active_ticks = 0;
  std::uint64_t frames = 0, callbacks = 0, fixed_steps = 0;
  std::uint64_t dropped_fixed_steps = 0, clamped_frames = 0, overruns = 0;
  std::uint64_t failed_ticks = 0, failed_frames = 0;
  Clock::duration max_work_time{};
};

// Host-thread frame scheduling composed with the MPSC EventLoop. The event
// loop outlives this instance. Heavy independent work belongs to Runtime.
class FrameLoop final {
public:
  explicit FrameLoop(EventLoop &events, FrameLoopOptions options = {});
  ~FrameLoop();
  FrameLoop(const FrameLoop &) = delete;
  FrameLoop &operator=(const FrameLoop &) = delete;
  FrameLoop(FrameLoop &&) = delete;
  FrameLoop &operator=(FrameLoop &&) = delete;

  [[nodiscard]] core::Result<TickToken> add(Tick tick, TickOptions options = {});
  [[nodiscard]] core::Result<void> remove(TickToken token);
  // Idempotently revoke registration and retire captures. Safe inside a tick;
  // its own capture retires after that invocation, and pending ticks are skipped.
  void close();
  [[nodiscard]] bool contains(TickToken token) const;
  [[nodiscard]] core::Result<void> set_enabled(TickToken token, bool enabled);
  [[nodiscard]] core::Result<void> add_dependency(TickToken dependent,
                                                TickToken prerequisite);
  [[nodiscard]] core::Result<void> remove_dependency(TickToken dependent,
                                                   TickToken prerequisite);

  // Pause/rate/enable/order changes take effect at the next frame boundary.
  // Removing a callback suppresses any delivery that has not begun.
  void set_paused(bool paused);
  [[nodiscard]] core::Result<void> set_time_scale(double scale);
  // Rebase after a host discontinuity; preserve elapsed time, clear backlog.
  void reset_clock(Clock::time_point now = Clock::now());

  // Frame only: safe from an EventLoop callback. First step has zero delta.
  FrameReport step(Clock::time_point now = Clock::now());
  // Host-driven event/I/O dispatch followed by one frame; never sleeps.
  FrameReport poll(PumpBudget budget = {});
  // Native timer-driven frames through EventLoop::run; no busy wait.
  void run(PumpBudget budget = {});
  [[nodiscard]] FrameStats stats() const;
  [[nodiscard]] EventLoop &events() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace ii::runtime
