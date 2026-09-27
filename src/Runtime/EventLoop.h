#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace ii::runtime {

using Clock = std::chrono::steady_clock;
using Task = std::function<void()>;
enum class Priority { high, normal, background };
enum class SubmitError { none, full, stopped, invalid };
enum class LoopState { ready, suspended, stopped };

// Opaque value identity; an expired token cannot cancel a reused slot.
class TaskToken final {
public:
    TaskToken() = default;
    friend bool operator==(const TaskToken&, const TaskToken&) = default;
private:
    friend class EventLoop;
    friend class Runtime;
    static std::uint64_t next_owner();
    std::uint64_t owner_ = 0;
    std::uint64_t generation_ = 0;
    std::size_t slot_ = 0;
};

struct Submission {
    TaskToken token;
    SubmitError error = SubmitError::none;
    explicit operator bool() const noexcept { return error == SubmitError::none; }
};

struct PumpBudget {
    std::size_t max_callbacks = 64;
    Clock::duration max_time = std::chrono::milliseconds(4);
};

struct LoopStats {
    std::size_t capacity = 0;
    std::size_t pending = 0;
    std::size_t high_watermark = 0;
    std::uint64_t submitted = 0;
    std::uint64_t executed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t rejected = 0;
    std::uint64_t missed_ticks = 0;
};

// One dispatch thread, multiple producers. Compose instances and callbacks;
// subclassing and owning raw pointers are not part of the public contract.
class EventLoop final {
public:
    explicit EventLoop(std::size_t capacity = 1024);
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;

    [[nodiscard]] Submission post(Task task, Priority priority = Priority::normal);
    [[nodiscard]] Submission at(Clock::time_point deadline, Task task,
                                Priority priority = Priority::normal);
    [[nodiscard]] Submission after(Clock::duration delay, Task task,
                                   Priority priority = Priority::normal);
    [[nodiscard]] Submission every(Clock::duration interval, Task task,
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
    std::unique_ptr<Impl> impl_;
    Submission submit(Clock::time_point, Clock::duration, Task, Priority);
    std::size_t dispatch(PumpBudget);
};

enum class Platform { macos, windows, linux_os, ios, android, wasm, unknown };
[[nodiscard]] Platform current_platform() noexcept;
[[nodiscard]] std::string_view platform_name() noexcept;

} // namespace ii::runtime
