#pragma once
#include "../Core/Time/Time.h"
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

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
  bool cancel() const {
    const auto owner = executor_.lock();
    return owner && cancel_ && cancel_(owner.get(), *this);
  }
  [[nodiscard]] bool pending() const {
    const auto owner = executor_.lock();
    return owner && pending_ && pending_(owner.get(), *this);
  }
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

// Structural execution contract shared by EventLoop, Runtime and SerialQueue.
// Admission, delivery and cancellation retain each executor's documented lifetime.
template<class T> concept TaskExecutor = requires(T& target, Task task, Priority priority) {
    { target.post(std::move(task), priority) } -> std::same_as<Submission>;
};
template<class T> concept TimedTaskExecutor = TaskExecutor<T> && requires(T& target, Clock::time_point deadline, Task task, Priority priority) {
    { target.at(deadline, std::move(task), priority) } -> std::same_as<Submission>;
};
} // namespace ii::runtime
