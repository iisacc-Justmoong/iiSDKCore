#pragma once

#include "Thread.h"

namespace ii::runtime {

struct RuntimeOptions {
    // 0 selects 2..4 workers from the host's hardware concurrency hint.
    std::size_t workers = 0;
    std::size_t capacity = 1024;
};
enum class RuntimeState { running, draining, cancelling, stopped };
enum class ShutdownMode { drain, cancel_pending };
struct RuntimeStats {
    std::size_t workers = 0, capacity = 0, pending = 0, active = 0, high_watermark = 0;
    std::uint64_t submitted = 0, executed = 0, cancelled = 0, rejected = 0, failed = 0;
    RuntimeState state = RuntimeState::running;
};

class SerialQueue;

// Starts persistent workers on construction. Submission is asynchronous and
// parallel by default; fixed task slots and workers are reused for every job.
class Runtime final {
public:
    explicit Runtime(RuntimeOptions options = {});
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    [[nodiscard]] Submission post(Task task, Priority priority = Priority::normal);
    bool cancel(TaskToken token); // queued jobs only; running work is not preempted
    void request_stop(ShutdownMode mode = ShutdownMode::drain);
    [[nodiscard]] ThreadResult join(); // closes admission, drains, joins all workers
    [[nodiscard]] ThreadResult wait_idle(); // keeps admission open; rejects own workers
    [[nodiscard]] RuntimeStats stats() const;

private:
    friend class SerialQueue;
    struct Lane;
    struct Shared;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    static Submission submit(const std::shared_ptr<Shared>&, const std::shared_ptr<Lane>&,
                             Task, Priority);
};

// One object's FIFO execution domain. It occupies a worker only while executing
// a job; other queues can execute concurrently on the same Runtime.
class SerialQueue final {
public:
    explicit SerialQueue(Runtime& runtime);
    ~SerialQueue();
    SerialQueue(const SerialQueue&) = delete;
    SerialQueue& operator=(const SerialQueue&) = delete;
    SerialQueue(SerialQueue&&) = delete;
    SerialQueue& operator=(SerialQueue&&) = delete;
    [[nodiscard]] Submission post(Task task, Priority priority = Priority::normal) const;
    [[nodiscard]] bool is_current() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ii::runtime
