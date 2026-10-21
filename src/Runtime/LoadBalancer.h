#pragma once

#include "Runtime.h"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace ii::runtime {

enum class BalancePolicy { round_robin, least_loaded };
enum class TargetHealth { initial, healthy, unhealthy };

struct LoadTarget final {
    std::uint64_t id=0;
    explicit operator bool() const noexcept { return id!=0; }
    friend bool operator==(LoadTarget,LoadTarget)=default;
};

struct LoadBalancerOptions final {
    std::size_t max_targets=16;
    std::size_t capacity=1024; // queued, executing and retiring jobs together
    BalancePolicy policy=BalancePolicy::least_loaded;
};

struct LoadTargetOptions final {
    std::string name;
    std::size_t capacity=1; // concurrent reserved work units, not OS threads
    std::size_t memory_capacity=0; // declared bytes; zero disables this limit
    std::uint64_t capabilities=0; // application-defined capability bits
    std::uint32_t unhealthy_threshold=3;
    std::uint32_t healthy_threshold=2;
    bool require_health_check=false;
};

struct LoadRequest final {
    std::size_t units=1;
    std::size_t memory_bytes=0;
    std::uint64_t capabilities=0;
    Priority priority=Priority::normal;
};

struct LoadTargetStats final {
    LoadTarget target;
    std::string name;
    std::size_t capacity=0, memory_capacity=0;
    std::uint64_t capabilities=0;
    std::size_t in_flight=0, units=0, memory_bytes=0;
    TargetHealth health=TargetHealth::initial;
    bool draining=false;
    std::uint64_t submitted=0, completed=0, failed=0, cancelled=0, rejected=0;
};

struct LoadBalancerStats final {
    std::size_t capacity=0, max_targets=0, targets=0, healthy_targets=0, draining_targets=0;
    std::size_t in_flight=0, high_watermark=0;
    std::uint64_t submitted=0, completed=0, failed=0, cancelled=0, rejected=0;
    bool stopped=false;
};

// Borrows existing native executors. No worker, network connection or interpreter
// is created here. Each executor must outlive its successful removal or close().
class LoadBalancer final {
public:
    explicit LoadBalancer(LoadBalancerOptions options={});
    ~LoadBalancer();
    LoadBalancer(const LoadBalancer&)=delete;
    LoadBalancer& operator=(const LoadBalancer&)=delete;
    LoadBalancer(LoadBalancer&&)=delete;
    LoadBalancer& operator=(LoadBalancer&&)=delete;

    template<TaskExecutor Executor>
    std::expected<LoadTarget,SubmitError> add_target(Executor& executor,LoadTargetOptions options={}) {
        return add_target_erased([&executor](Task task,Priority priority) {
            return executor.post(std::move(task),priority);
        },std::move(options));
    }

    Submission post(Task task,Priority priority=Priority::normal);
    Submission submit(Task task,LoadRequest request={});
    // Consecutive health observations are supplied by the owner/monitor. No
    // blocking probe runs on the dispatch path. Existing jobs keep their target.
    bool report_health(LoadTarget target,bool healthy);
    bool request_drain(LoadTarget target);
    ThreadResult remove_target(LoadTarget target,ShutdownMode mode=ShutdownMode::cancel_pending);
    void request_stop(); // stop admission; retain accepted work for draining
    ThreadResult wait_idle() const;
    ThreadResult close(ShutdownMode mode=ShutdownMode::cancel_pending);
    LoadBalancerStats stats() const;
    std::optional<LoadTargetStats> target_stats(LoadTarget target) const;

private:
    struct Impl;
    struct Target;
    struct Ticket;
    std::shared_ptr<Impl> impl_;
    using SubmitFunction=std::function<Submission(Task,Priority)>;
    std::expected<LoadTarget,SubmitError> add_target_erased(SubmitFunction submit,LoadTargetOptions options);
};
} // namespace ii::runtime
