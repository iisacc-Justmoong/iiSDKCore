#pragma once

#include "Thread.h"
#include "Session.h"
#include <vector>

namespace ii::runtime {

enum class RuntimeWorkerPolicy { bounded, hardware, none };
struct RuntimeOptions final {
    // Zero starts no workers unless an explicit automatic policy is selected.
    std::size_t workers = 0;
    std::size_t capacity = 1024;
    // Zero uses min(workers, capacity), or one queue while dormant.
    std::size_t queues = 0;
    // Used only when workers is zero; automatic sizing is bounded by capacity.
    RuntimeWorkerPolicy worker_policy = RuntimeWorkerPolicy::none;
};
enum class RuntimeState { running, draining, cancelling, stopped };
enum class ShutdownMode { drain, cancel_pending };
struct RuntimeStats final {
    std::size_t workers = 0, capacity = 0, pending = 0, active = 0, high_watermark = 0;
    std::uint64_t submitted = 0, executed = 0, cancelled = 0, rejected = 0, failed = 0;
    RuntimeState state = RuntimeState::running;
    std::size_t queues = 0;
};
struct RuntimeQueueStats final {
    std::size_t index = 0, capacity = 0, pending = 0, ready = 0, active = 0;
    std::uint64_t submitted = 0, executed = 0, cancelled = 0, cross_queue = 0;
};

class SerialQueue;

// Starts no workers by default. Explicitly configured workers execute submitted
// tasks asynchronously and reuse their bounded slots until released.
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
    // Retire workers cooperatively or add workers without replacing the queue.
    // Zero drains accepted work and releases all workers until a later resize.
    [[nodiscard]] ThreadResult resize_workers(std::size_t count);
    void request_stop(ShutdownMode mode = ShutdownMode::drain);
    [[nodiscard]] ThreadResult join(); // closes admission, drains, joins all workers
    [[nodiscard]] ThreadResult wait_idle(); // keeps admission open; rejects own workers
    [[nodiscard]] RuntimeStats stats() const;
    [[nodiscard]] std::size_t allocated_slots() const;
    [[nodiscard]] std::vector<RuntimeQueueStats> queue_stats() const;
    [[nodiscard]] static bool is_worker_thread() noexcept;
    // Runtime-owned objects remain alive through queued and running work.
    [[nodiscard]] ObjectOwner& objects() const noexcept;
    // Creation is rejected with std::logic_error once shutdown has begun.
    [[nodiscard]] std::unique_ptr<Session> create_session();

private:
    friend class SerialQueue;
    friend class Session;
    friend class ComputeRouter;
    struct Lane;
    struct Shared;
    struct Impl;
    struct StatsSubscription;
    std::unique_ptr<Impl> impl_;
    static Submission submit(const std::shared_ptr<Shared>&, const std::shared_ptr<Lane>&,
                             Task, Priority);
    static bool cancel_shared(void*, const TaskToken&);
    static bool pending_shared(void*, const TaskToken&);
    static bool wait_shared(void*, const TaskToken&);
    std::shared_ptr<StatsSubscription> observe_stats(Task) const;
    void remove_stats_observer(const std::shared_ptr<StatsSubscription>&) const noexcept;
    std::uint64_t stats_revision() const noexcept;
    std::pair<RuntimeStats,std::uint64_t> versioned_stats() const;
    void register_session(const std::shared_ptr<Session::State>&);
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
    // Cancel this object's queued work and await active callbacks and captures.
    void request_close() const;
    [[nodiscard]] ThreadResult close() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ii::runtime
