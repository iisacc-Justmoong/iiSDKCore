#pragma once
#include "LoadBalancer.h"
#include "Operation.h"
#include "../Process/Process.h"
#include <atomic>
#include <span>
#include <vector>

namespace ii::runtime {
struct AvailabilityZone final {
    std::uint64_t id=0;
    explicit operator bool() const noexcept { return id!=0; }
    friend bool operator==(AvailabilityZone,AvailabilityZone)=default;
};
struct ComputeScope final {
    std::uint64_t id=0;
    explicit operator bool() const noexcept { return id!=0; }
    friend bool operator==(ComputeScope,ComputeScope)=default;
};
struct ComputeLimits final {
    // Zero leaves this dimension unlimited; the router's global bound remains.
    std::size_t jobs=0,units=0,memory_bytes=0;
};
struct AvailabilityZoneOptions final { std::string name={}; bool enabled=true; ComputeLimits limits={}; };
struct ComputeScopeOptions final {
    std::string name={};
    std::vector<AvailabilityZone> zones={}; // empty covers all current/future zones
    bool enabled=true;
    ComputeLimits limits={};
};
struct ComputeRouterOptions final { std::size_t max_zones=16,max_scopes=32,max_targets=64,capacity=1024; };
struct ComputeTargetOptions final {
    LoadTargetOptions load{.name={},.capacity=0}; // zero derives Runtime queue capacity
    bool require_observation=false;
    double cpu_ceiling=1.0;
};
struct ComputeObservation final {
    std::optional<double> cpu_utilization={};
    std::size_t external_memory_bytes=0,memory_capacity=0;
    Clock::time_point sampled_at{};
    Clock::duration valid_for=std::chrono::seconds(1);
};
struct ComputeRequest final {
    LoadRequest work={};
    std::optional<AvailabilityZone> preferred_zone={};
    bool cross_zone=true;
};
struct ComputePlacement final { LoadTarget target; AvailabilityZone zone; ComputeScope scope; };
struct ComputeUsage final {
    std::size_t in_flight=0,units=0,memory_bytes=0;
    std::uint64_t acquired=0,completed=0,failed=0,cancelled=0;
};
struct AvailabilityZoneSnapshot final {
    AvailabilityZone zone;
    AvailabilityZoneOptions options;
    ComputeUsage usage;
    std::size_t targets=0;
    bool overcommitted=false;
};
struct ComputeScopeSnapshot final { ComputeScope scope; ComputeScopeOptions options; ComputeUsage usage; bool overcommitted=false; };
struct ComputeTargetSnapshot final {
    LoadTarget target;
    AvailabilityZone zone;
    LoadTargetStats load;
    RuntimeStats runtime;
    Clock::time_point runtime_sampled_at{};
    ComputeUsage usage;
    std::optional<ComputeObservation> observation={};
    bool observation_fresh=false,enabled=true,retiring=false;
    std::size_t active_callbacks=0,active_processes=0;
    std::uint64_t callbacks_started=0,callbacks_finished=0;
    Clock::duration queue_time{},execution_time{};
};
struct ComputeSnapshot final {
    Clock::time_point sampled_at{};
    std::uint64_t revision=0,monitor_samples=0,rejected=0;
    bool stopped=false;
    LoadBalancerStats load;
    std::vector<AvailabilityZoneSnapshot> zones;
    std::vector<ComputeScopeSnapshot> scopes;
    std::vector<ComputeTargetSnapshot> targets;
};
class ComputeRouter;
class ComputeLease final {
public:
    ComputeLease() noexcept=default;
    ~ComputeLease();
    ComputeLease(ComputeLease&&) noexcept;
    ComputeLease& operator=(ComputeLease&&) noexcept;
    ComputeLease(const ComputeLease&)=delete;
    ComputeLease& operator=(const ComputeLease&)=delete;
    explicit operator bool() const noexcept { return bool(state_); }
    ComputePlacement placement() const noexcept;
    std::stop_token stop_token() const noexcept;
    Submission post(Task,Priority=Priority::normal) const;
    void complete() noexcept;
    void fail(std::exception_ptr={}) noexcept;
    void cancel() noexcept;
private:
    friend class ComputeRouter;
    struct State;
    explicit ComputeLease(std::shared_ptr<State> state):state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};
class RoutedProcess final {
public:
    ComputePlacement placement() const noexcept { return placement_; }
    std::uint64_t native_id() const noexcept { return native_id_->load(); }
    const Operation<process::Communication>& operation() const noexcept { return operation_; }
private:
    friend class ComputeRouter;
    RoutedProcess(ComputePlacement placement,Operation<process::Communication> operation,std::shared_ptr<std::atomic<std::uint64_t>> id)
        :placement_(placement),operation_(std::move(operation)),native_id_(std::move(id)) {}
    ComputePlacement placement_;
    Operation<process::Communication> operation_;
    std::shared_ptr<std::atomic<std::uint64_t>> native_id_;
};
class ComputeRouter final {
public:
    explicit ComputeRouter(ComputeRouterOptions={});
    ~ComputeRouter();
    ComputeRouter(const ComputeRouter&)=delete;
    ComputeRouter& operator=(const ComputeRouter&)=delete;
    std::expected<AvailabilityZone,SubmitError> add_zone(AvailabilityZoneOptions);
    std::expected<void,SubmitError> configure_zone(AvailabilityZone,AvailabilityZoneOptions);
    std::expected<void,SubmitError> remove_zone(AvailabilityZone);
    std::expected<ComputeScope,SubmitError> add_scope(ComputeScopeOptions);
    std::expected<void,SubmitError> configure_scope(ComputeScope,ComputeScopeOptions);
    std::expected<void,SubmitError> remove_scope(ComputeScope);
    ComputeScope default_scope() const noexcept;
    std::expected<LoadTarget,SubmitError> add_target(Runtime&,AvailabilityZone,ComputeTargetOptions={});
    std::expected<void,SubmitError> assign_target(LoadTarget,AvailabilityZone);
    std::expected<void,SubmitError> enable_target(LoadTarget,bool);
    bool report_health(LoadTarget,bool);
    bool report_load(LoadTarget,ComputeObservation);
    std::expected<ComputeLease,SubmitError> acquire(ComputeScope,ComputeRequest={});
    Submission post(Task,Priority=Priority::normal);
    Submission submit(ComputeScope,Task,ComputeRequest={});
    std::expected<RoutedProcess,SubmitError> route_process(ComputeScope,process::Launch,
        core::Bytes input={},process::CommunicationOptions={},ComputeRequest={});
    // Reads native Runtime counters now. No OS CPU/RSS values are fabricated;
    // optional external observations retain their timestamp and freshness.
    ComputeSnapshot monitor();
    Submission start_monitoring(EventLoop&,Clock::duration interval=std::chrono::milliseconds(100));
    ThreadResult stop_monitoring();
    ThreadResult remove_target(LoadTarget,ShutdownMode=ShutdownMode::cancel_pending);
    ThreadResult close(ShutdownMode=ShutdownMode::cancel_pending);
private:
    friend class ComputeLease;
    struct Impl;
    std::shared_ptr<Impl> impl_;
    std::expected<ComputeLease,SubmitError> allocate(ComputeScope,ComputeRequest,bool external);
};
} // namespace ii::runtime
