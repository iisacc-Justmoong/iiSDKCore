#pragma once

#include "../Core/Time/Time.h"
#include "../IO/Requests.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace ii::runtime {

using Clock = core::Clock;
using Task = std::function<void()>;
enum class Priority { high, normal, background };
enum class SubmitError { none, full, stopped, invalid, overloaded };
enum class LoopState { ready, suspended, stopped };

// Opaque value identity; an expired token cannot cancel a reused slot.
class TaskToken final {
public:
  TaskToken() = default;
  // Lifetime-safe cancellation even when the submitting executor was removed.
  bool cancel() const;
  [[nodiscard]] bool pending() const;
  friend bool operator==(const TaskToken &a, const TaskToken &b) {
    return a.owner_ == b.owner_ && a.generation_ == b.generation_ &&
           a.slot_ == b.slot_;
  }

private:
  friend class EventLoop;
  friend class Runtime;
  friend class SerialQueue;
  static std::uint64_t next_owner();
  std::uint64_t owner_ = 0;
  std::uint64_t generation_ = 0;
  std::size_t slot_ = 0;
  std::weak_ptr<void> executor_;
  bool (*cancel_)(void *, const TaskToken &) = nullptr;
  bool (*pending_)(void *, const TaskToken &) = nullptr;
};

struct Submission final {
  TaskToken token;
  SubmitError error = SubmitError::none;
  explicit operator bool() const noexcept { return error == SubmitError::none; }
};

struct PumpBudget final {
  std::size_t max_callbacks = 64;
  Clock::duration max_time = std::chrono::milliseconds(4);
};

enum class SliceResult { complete, yield, stop };
using SliceTask = std::function<SliceResult(const core::TimeSlice &)>;
struct RealtimeSchedule final {
  Clock::duration period = std::chrono::milliseconds(10);
  Clock::duration deadline = std::chrono::milliseconds(10);
  Clock::duration runtime = std::chrono::milliseconds(2);
  Clock::duration quantum = std::chrono::microseconds(250);
  std::optional<Clock::time_point> start;
};

struct LoopStats final {
  std::size_t capacity = 0;
  std::size_t pending = 0;
  std::size_t high_watermark = 0;
  std::uint64_t submitted = 0;
  std::uint64_t executed = 0;
  std::uint64_t cancelled = 0;
  std::uint64_t rejected = 0;
  std::uint64_t missed_ticks = 0;
  std::uint64_t io_waits = 0;
  std::uint64_t slices = 0, slice_yields = 0, slice_overruns = 0;
  std::uint64_t deadline_misses = 0, skipped_releases = 0,
                budget_exhaustions = 0;
  Clock::duration max_lateness{};
  double reserved_utilization = 0;
};

using IOReady = std::function<void(io::Result<void>)>;
using IOCompletion = std::function<void(io::IOResult)>;
struct IOWatchOptions final {
  std::optional<Clock::duration>
      timeout; // none waits until readiness/cancellation
  io::CancellationToken stop;
  Priority priority = Priority::normal;
};

// One dispatch thread, multiple producers. Compose instances and callbacks;
// subclassing and owning raw pointers are not part of the public contract.
class EventLoop final {
public:
  explicit EventLoop(std::size_t capacity = 1024);
  ~EventLoop();
  EventLoop(const EventLoop &) = delete;
  EventLoop &operator=(const EventLoop &) = delete;
  EventLoop(EventLoop &&) = delete;
  EventLoop &operator=(EventLoop &&) = delete;

  [[nodiscard]] Submission post(Task task,
                                Priority priority = Priority::normal);
  [[nodiscard]] Submission at(Clock::time_point deadline, Task task,
                              Priority priority = Priority::normal);
  [[nodiscard]] Submission after(Clock::duration delay, Task task,
                                 Priority priority = Priority::normal);
  [[nodiscard]] Submission every(Clock::duration interval, Task task,
                                 Priority priority = Priority::normal);
  // EDF reservations; yielding continues this period, complete waits for the
  // next release, stop retires. No kernel preemption or CPU-time guarantee.
  [[nodiscard]] Submission realtime(RealtimeSchedule schedule, SliceTask task);
  // One-shot readiness/transfer completion on the loop's dispatch thread.
  // Read/Write timeout starts at admission; zero is a readiness probe.
  [[nodiscard]] Submission watch(io::Channel channel, io::Interest interest,
                                 IOReady ready, IOWatchOptions options = {});
  [[nodiscard]] Submission read(io::Channel channel, io::Read request,
                                IOCompletion completion,
                                Priority priority = Priority::normal);
  [[nodiscard]] Submission write(io::Channel channel, io::Write request,
                                 IOCompletion completion,
                                 Priority priority = Priority::normal);
  bool cancel(TaskToken token);

  // Nonblocking host-driven dispatch for UIKit, Android, GUI hosts and WASM.
  std::size_t poll(PumpBudget budget = {});
  // Blocking native/pthread worker driver. The browser host thread rejects it.
  void run(PumpBudget budget = {});
  void suspend();
  void resume();
  void request_stop();
  [[nodiscard]] LoopState state() const;
  [[nodiscard]] LoopStats stats() const;
  [[nodiscard]] std::optional<Clock::time_point> next_deadline() const;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  static bool cancel_impl(void *, const TaskToken &);
  static bool pending_impl(void *, const TaskToken &);
  Submission submit(Clock::time_point, Clock::duration, Task, Priority);
  Submission submit_io(io::Channel, io::Interest, IOReady, IOCompletion,
                       std::variant<std::monostate, io::Read, io::Write>,
                       IOWatchOptions);
  std::size_t dispatch(PumpBudget);
};

enum class Platform { macos, windows, linux_os, ios, android, wasm, unknown };
[[nodiscard]] Platform current_platform() noexcept;
[[nodiscard]] std::string_view platform_name() noexcept;

} // namespace ii::runtime
