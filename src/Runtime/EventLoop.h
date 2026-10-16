#pragma once

#include "Execution.h"
#include "../IO/Requests.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace ii::runtime {
class HostLoop;

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
  struct Impl;
public:
  // A weak producer endpoint. It never retains the EventLoop owner or allows
  // dispatch; accepted jobs retain the same bounded slots and cancellation.
  class Sender final {
  public:
    Sender() = default;
    [[nodiscard]] Submission post(Task task, Priority priority = Priority::normal) const;
    [[nodiscard]] Submission at(Clock::time_point, Task, Priority = Priority::normal) const;
    [[nodiscard]] Submission after(Clock::duration, Task, Priority = Priority::normal) const;
    [[nodiscard]] Submission every(Clock::duration, Task, Priority = Priority::normal) const;
    [[nodiscard]] Submission realtime(RealtimeSchedule, SliceTask) const;
    [[nodiscard]] Submission watch(io::Channel, io::Interest, IOReady, IOWatchOptions = {}) const;
    [[nodiscard]] Submission read(io::Channel, io::Read, IOCompletion, Priority = Priority::normal) const;
    [[nodiscard]] Submission write(io::Channel, io::Write, IOCompletion, Priority = Priority::normal) const;
    bool cancel(TaskToken) const;
  private:
    friend class EventLoop;
    explicit Sender(const std::shared_ptr<Impl>& state) : state_(state) {}
    std::weak_ptr<Impl> state_;
  };
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
  [[nodiscard]] Sender sender() const noexcept { return Sender(impl_); }

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
  friend class HostLoop;
  std::shared_ptr<Impl> impl_;
  static bool cancel_impl(void *, const TaskToken &);
  static bool pending_impl(void *, const TaskToken &);
  static Submission submit(const std::shared_ptr<Impl>&, Clock::time_point, Clock::duration, Task, Priority);
  static Submission submit_realtime(const std::shared_ptr<Impl>&, RealtimeSchedule, SliceTask);
  static Submission submit_every(const std::shared_ptr<Impl>&, Clock::duration, Task, Priority);
  static Submission submit_io(const std::shared_ptr<Impl>&, io::Channel, io::Interest, IOReady, IOCompletion,
                       std::variant<std::monostate, io::Read, io::Write>,
                       IOWatchOptions);
  std::size_t dispatch(PumpBudget);
};

enum class Platform { macos, windows, linux_os, ios, android, wasm, unknown };
[[nodiscard]] Platform current_platform() noexcept;
[[nodiscard]] std::string_view platform_name() noexcept;

} // namespace ii::runtime
