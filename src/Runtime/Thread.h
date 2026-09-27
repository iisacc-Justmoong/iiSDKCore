#pragma once

#include "EventLoop.h"
#include <exception>
#include <expected>
#include <thread>

namespace ii::runtime {

enum class ThreadState { idle, running, stop_requested, completed, failed };
enum class ThreadErrorCode {
    unsupported, invalid_work, busy, self_join, would_block,
    start_failed, join_failed, work_failed
};
struct ThreadError {
    ThreadErrorCode code;
    std::exception_ptr cause;
};
using ThreadResult = std::expected<void, ThreadError>;

// Borrowed only for the duration of the work callback. Do not retain this
// reference after returning. Each joined/restarted run gets a fresh stop state.
class ThreadContext final {
public:
    ThreadContext(const ThreadContext&) = delete;
    ThreadContext& operator=(const ThreadContext&) = delete;
    [[nodiscard]] bool stop_requested() const;
    // Returns true on a stop request, false when the duration has elapsed.
    bool wait_for(Clock::duration duration) const;
private:
    friend class Thread;
    struct State;
    explicit ThreadContext(State& state) : state_(state) {}
    State& state_;
};

// Owns one worker at a time. No inheritance, native handle or detach operation.
// Lifecycle methods are synchronized; object destruction still requires that
// no other caller is using this object, and must not occur on its own worker.
class Thread final {
public:
    using Work = std::function<void(ThreadContext&)>;
    using Id = std::thread::id;
    Thread();
    ~Thread();
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&&) = delete;
    Thread& operator=(Thread&&) = delete;

    [[nodiscard]] ThreadResult start(Work work);
    // loop must outlive this run; request_stop also wakes the loop's native wait.
    [[nodiscard]] ThreadResult start(EventLoop& loop, PumpBudget budget = {});
    bool request_stop();
    // Joins without requesting stop. Repeated joins retain the last work result.
    [[nodiscard]] ThreadResult join();
    [[nodiscard]] bool joinable() const;
    [[nodiscard]] ThreadState state() const;
    [[nodiscard]] Id id() const;
    [[nodiscard]] static Id current_id() noexcept;
    [[nodiscard]] static bool supported() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ii::runtime
