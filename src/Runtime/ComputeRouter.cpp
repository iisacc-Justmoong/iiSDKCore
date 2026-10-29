#include "ComputeRouter.h"
#include "../Core/Text/Utf8.h"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <stdexcept>
#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
#endif

namespace ii::runtime {
namespace {
struct Activity final {
    const void* owner;
    Activity* previous;
    static thread_local Activity* current;
    explicit Activity(const void* value):owner(value),previous(current) { current=this; }
    ~Activity() { current=previous; }
    static bool contains(const void* value) {
        for(auto* entry=current;entry;entry=entry->previous) if(entry->owner==value) return true;
        return false;
    }
};
thread_local Activity* Activity::current=nullptr;
ThreadResult error(ThreadErrorCode code) { return std::unexpected(ThreadError{code,{}}); }
bool cannot_block() {
#if defined(__EMSCRIPTEN__)
    if(emscripten_is_main_browser_thread()) return true;
#endif
    return Runtime::is_worker_thread() || HostLoop::is_current_thread();
}
std::uint64_t identity() {
    static std::atomic<std::uint64_t> next{1};
    auto value=next.load();
    do {
        if(value==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Compute topology identities exhausted");
    } while(!next.compare_exchange_weak(value,value+1));
    return value;
}
bool name_valid(const std::string& name) { return !name.empty() && name.size()<=256 && name.find('\0')==name.npos && core::valid_utf8(name); }
bool fits(std::size_t used,std::size_t added,std::size_t limit=0) {
    if(!limit) limit=std::numeric_limits<std::size_t>::max();
    return used<=limit && added<=limit-used;
}
bool fits(const ComputeUsage& usage,const LoadRequest& request,const ComputeLimits& limits) {
    return fits(usage.in_flight,1,limits.jobs) && fits(usage.units,request.units,limits.units) &&
        fits(usage.memory_bytes,request.memory_bytes,limits.memory_bytes);
}
bool overcommitted(const ComputeUsage& usage,const ComputeLimits& limits) {
    return (limits.jobs && usage.in_flight>limits.jobs) || (limits.units && usage.units>limits.units) ||
        (limits.memory_bytes && usage.memory_bytes>limits.memory_bytes);
}
void reserve(ComputeUsage& usage,const LoadRequest& request) {
    ++usage.in_flight; usage.units+=request.units; usage.memory_bytes+=request.memory_bytes; ++usage.acquired;
}
void add_time(Clock::duration& total,Clock::duration value) {
    total=value>Clock::duration::max()-total ? Clock::duration::max() : total+value;
}
bool fresh(const std::optional<ComputeObservation>& observation,Clock::time_point now) {
    return observation && observation->sampled_at<=now && core::Time::elapsed(observation->sampled_at,now)<observation->valid_for;
}
}
struct ComputeRouter::Impl final {
    struct Zone final { AvailabilityZone id; AvailabilityZoneOptions options; ComputeUsage usage; };
    struct Scope final { ComputeScope id; ComputeScopeOptions options; ComputeUsage usage; };
    struct Target final {
        LoadTarget id;
        Runtime* runtime=nullptr;
        std::shared_ptr<Zone> zone;
        ComputeTargetOptions options;
        ComputeUsage usage;
        RuntimeStats sample;
        Clock::time_point sampled_at{};
        LoadTargetStats load;
        std::optional<ComputeObservation> observation;
        bool enabled=true,retiring=false;
        std::size_t external=0,active_callbacks=0,active_processes=0;
        std::uint64_t callbacks_started=0,callbacks_finished=0;
        Clock::duration queue_time{},execution_time{};
    };
    struct Monitor final { TaskScope calls; TaskToken token; };
    const ComputeRouterOptions options;
    mutable std::mutex mutex;
    std::mutex lifecycle;
    std::condition_variable changed;
    std::vector<std::shared_ptr<Zone>> zones;
    std::vector<std::shared_ptr<Scope>> scopes;
    std::vector<std::shared_ptr<Target>> targets;
    std::vector<LoadTarget> candidates;
    std::shared_ptr<Monitor> monitoring;
    ComputeScope default_scope;
    bool stopped=false;
    std::size_t external=0,internal=0;
    std::uint64_t revision=0,samples=0,rejected=0;
    LoadBalancer balance;
    TaskScope work;
    OperationOwner processes;
    explicit Impl(ComputeRouterOptions settings)
        :options(settings),zones(settings.max_zones),scopes(settings.max_scopes),targets(settings.max_targets),
         balance({.max_targets=settings.max_targets,.capacity=settings.capacity}) {
        if(!settings.max_zones || !settings.max_scopes) throw std::invalid_argument("ComputeRouter requires positive topology bounds");
        auto initial=std::make_shared<Scope>();
        initial->id={identity()}; initial->options.name="default"; default_scope=initial->id; scopes.front()=std::move(initial);
        candidates.reserve(settings.max_targets);
    }
    template<class Collection,class Id> static auto find(const Collection& collection,Id id) {
        using Value=typename Collection::value_type;
        for(const auto& value:collection) if(value && value->id==id) return value;
        return Value{};
    }
    bool scope_valid(const ComputeScopeOptions& value,const std::shared_ptr<Scope>& replacing={}) const {
        if(!name_valid(value.name) || value.zones.size()>options.max_zones) return false;
        for(const auto& scope:scopes) if(scope && scope!=replacing && scope->options.name==value.name) return false;
        for(std::size_t i=0;i<value.zones.size();++i) {
            if(!find(zones,value.zones[i])) return false;
            for(std::size_t j=0;j<i;++j) if(value.zones[i]==value.zones[j]) return false;
        }
        return true;
    }
    bool covers(const Scope& scope,AvailabilityZone zone) const {
        return scope.options.zones.empty() || std::find(scope.options.zones.begin(),scope.options.zones.end(),zone)!=scope.options.zones.end();
    }
    void refresh_locked() {
        const auto now=Clock::now();
        for(const auto& target:targets) if(target) {
            // Only the built-in Runtime is sampled here. Its stats() takes no
            // callbacks and Runtime retires captures outside its own mutex.
            if(target->runtime) { target->sample=target->runtime->stats(); target->sampled_at=now; }
            if(const auto load=balance.target_stats(target->id)) target->load=*load;
            else {
                target->load.in_flight=target->usage.in_flight; target->load.units=target->usage.units;
                target->load.memory_bytes=target->usage.memory_bytes; target->load.reservations=target->usage.in_flight;
                target->load.submitted=target->usage.acquired; target->load.completed=target->usage.completed;
                target->load.failed=target->usage.failed; target->load.cancelled=target->usage.cancelled;
            }
        }
        ++samples;
    }
    void rejected_request() { std::lock_guard lock(mutex); ++rejected; }
};

struct ComputeLease::State final {
    enum class Outcome { cancelled,completed,failed };
    std::shared_ptr<ComputeRouter::Impl> owner;
    std::shared_ptr<ComputeRouter::Impl::Zone> zone;
    std::shared_ptr<ComputeRouter::Impl::Scope> scope;
    std::shared_ptr<ComputeRouter::Impl::Target> target;
    LoadLease lease;
    ComputePlacement placement;
    LoadRequest resources;
    std::atomic<Outcome> outcome{Outcome::cancelled};
    std::exception_ptr cause;
    bool external=false,accounted=false;
    explicit State(std::shared_ptr<ComputeRouter::Impl> value):owner(std::move(value)) {}
    ~State() {
        if(!accounted) return;
        Activity retiring(owner.get());
        const auto result=outcome.load();
        if(result==Outcome::completed) lease.complete();
        else if(result==Outcome::failed) lease.fail(cause);
        else lease.cancel();
        std::lock_guard lock(owner->mutex);
        for(auto* usage:{&zone->usage,&scope->usage,&target->usage}) {
            --usage->in_flight; usage->units-=resources.units; usage->memory_bytes-=resources.memory_bytes;
            if(result==Outcome::completed) ++usage->completed;
            else if(result==Outcome::failed) ++usage->failed;
            else ++usage->cancelled;
        }
        if(external) { --owner->external; --target->external; }
        else --owner->internal;
        owner->changed.notify_all();
    }
    void complete() noexcept {
        auto expected=Outcome::cancelled;
        (void)outcome.compare_exchange_strong(expected,Outcome::completed);
    }
    void fail(std::exception_ptr failure={}) noexcept {
        std::lock_guard lock(owner->mutex);
        if(!cause) cause=std::move(failure);
        outcome=Outcome::failed;
    }
    struct Delivery final {
        std::shared_ptr<State> state;
        Task task;
        Clock::time_point queued=Clock::now();
        Delivery(std::shared_ptr<State> owner,Task value):state(std::move(owner)),task(std::move(value)) {}
        ~Delivery() { Activity retiring(state->owner.get()); task=nullptr; }
        void run() {
            Activity active(state->owner.get());
            const auto started=Clock::now();
            {
                std::lock_guard lock(state->owner->mutex);
                ++state->target->active_callbacks; ++state->target->callbacks_started;
                add_time(state->target->queue_time,core::Time::elapsed(queued,started));
            }
            struct Finish final {
                State& state;
                Clock::time_point started;
                ~Finish() {
                    std::lock_guard lock(state.owner->mutex);
                    --state.target->active_callbacks; ++state.target->callbacks_finished;
                    add_time(state.target->execution_time,core::Time::elapsed(started,Clock::now()));
                }
            } finish{*state,started};
            try { task(); } catch(...) { state->fail(std::current_exception()); throw; }
        }
    };
};

ComputeRouter::ComputeRouter(ComputeRouterOptions options):impl_(std::make_shared<Impl>(options)) {}
ComputeRouter::~ComputeRouter() {
    const auto result=close();
    if(!result && result.error().code!=ThreadErrorCode::work_failed) std::terminate();
}
ComputeScope ComputeRouter::default_scope() const noexcept { return impl_->default_scope; }
std::expected<AvailabilityZone,SubmitError> ComputeRouter::add_zone(AvailabilityZoneOptions options) {
    std::lock_guard lock(impl_->mutex);
    if(impl_->stopped) return std::unexpected(SubmitError::stopped);
    if(!name_valid(options.name)) return std::unexpected(SubmitError::invalid);
    for(const auto& zone:impl_->zones) if(zone && zone->options.name==options.name) return std::unexpected(SubmitError::invalid);
    const auto slot=std::find(impl_->zones.begin(),impl_->zones.end(),nullptr);
    if(slot==impl_->zones.end()) return std::unexpected(SubmitError::full);
    auto zone=std::make_shared<Impl::Zone>(); zone->id={identity()}; zone->options=std::move(options);
    *slot=zone; ++impl_->revision; return zone->id;
}
std::expected<void,SubmitError> ComputeRouter::configure_zone(AvailabilityZone id,AvailabilityZoneOptions options) {
    std::lock_guard lock(impl_->mutex);
    if(impl_->stopped) return std::unexpected(SubmitError::stopped);
    const auto zone=Impl::find(impl_->zones,id);
    if(!zone || !name_valid(options.name)) return std::unexpected(SubmitError::invalid);
    for(const auto& other:impl_->zones) if(other && other!=zone && other->options.name==options.name) return std::unexpected(SubmitError::invalid);
    zone->options=std::move(options); ++impl_->revision; return {};
}
std::expected<void,SubmitError> ComputeRouter::remove_zone(AvailabilityZone id) {
    std::lock_guard lock(impl_->mutex);
    if(impl_->stopped) return std::unexpected(SubmitError::stopped);
    const auto zone=Impl::find(impl_->zones,id);
    if(!zone) return std::unexpected(SubmitError::invalid);
    if(zone->usage.in_flight) return std::unexpected(SubmitError::overloaded);
    for(const auto& target:impl_->targets) if(target && target->zone==zone) return std::unexpected(SubmitError::overloaded);
    for(const auto& scope:impl_->scopes) if(scope && !scope->options.zones.empty() && impl_->covers(*scope,id))
        return std::unexpected(SubmitError::overloaded);
    for(auto& slot:impl_->zones) if(slot==zone) { slot.reset(); break; }
    ++impl_->revision; return {};
}
std::expected<ComputeScope,SubmitError> ComputeRouter::add_scope(ComputeScopeOptions options) {
    std::lock_guard lock(impl_->mutex);
    if(impl_->stopped) return std::unexpected(SubmitError::stopped);
    if(!impl_->scope_valid(options)) return std::unexpected(SubmitError::invalid);
    const auto slot=std::find(impl_->scopes.begin(),impl_->scopes.end(),nullptr);
    if(slot==impl_->scopes.end()) return std::unexpected(SubmitError::full);
    auto scope=std::make_shared<Impl::Scope>(); scope->id={identity()}; scope->options=std::move(options);
    *slot=scope; ++impl_->revision; return scope->id;
}
std::expected<void,SubmitError> ComputeRouter::configure_scope(ComputeScope id,ComputeScopeOptions options) {
    std::lock_guard lock(impl_->mutex);
    if(impl_->stopped) return std::unexpected(SubmitError::stopped);
    const auto scope=Impl::find(impl_->scopes,id);
    if(!scope || !impl_->scope_valid(options,scope)) return std::unexpected(SubmitError::invalid);
    scope->options=std::move(options); ++impl_->revision; return {};
}
std::expected<void,SubmitError> ComputeRouter::remove_scope(ComputeScope id) {
    std::lock_guard lock(impl_->mutex);
    if(impl_->stopped) return std::unexpected(SubmitError::stopped);
    const auto scope=Impl::find(impl_->scopes,id);
    if(!scope || id==impl_->default_scope) return std::unexpected(SubmitError::invalid);
    if(scope->usage.in_flight) return std::unexpected(SubmitError::overloaded);
    for(auto& slot:impl_->scopes) if(slot==scope) { slot.reset(); break; }
    ++impl_->revision; return {};
}
std::expected<LoadTarget,SubmitError> ComputeRouter::add_target(Runtime& runtime,AvailabilityZone id,ComputeTargetOptions options) {
    const auto sample=runtime.stats();
    if(!options.load.capacity) options.load.capacity=sample.capacity;
    if(!std::isfinite(options.cpu_ceiling) || options.cpu_ceiling<=0 || options.cpu_ceiling>1 ||
       (!options.load.name.empty() && !name_valid(options.load.name)))
        return std::unexpected(SubmitError::invalid);
    auto target=std::make_shared<Impl::Target>();
    target->runtime=&runtime; target->sample=sample; target->sampled_at=Clock::now(); target->options=options;
    std::lock_guard lock(impl_->mutex);
    if(impl_->stopped) return std::unexpected(SubmitError::stopped);
    target->zone=Impl::find(impl_->zones,id);
    if(!target->zone) return std::unexpected(SubmitError::invalid);
    for(const auto& other:impl_->targets) if(other && other->runtime==&runtime) return std::unexpected(SubmitError::invalid);
    const auto slot=std::find(impl_->targets.begin(),impl_->targets.end(),nullptr);
    if(slot==impl_->targets.end()) return std::unexpected(SubmitError::full);
    const auto registered=impl_->balance.add_target(runtime,std::move(options.load));
    if(!registered) return std::unexpected(registered.error());
    target->id=*registered; target->load=*impl_->balance.target_stats(*registered); *slot=target;
    ++impl_->revision; return *registered;
}
std::expected<void,SubmitError> ComputeRouter::assign_target(LoadTarget id,AvailabilityZone zone_id) {
    std::lock_guard lock(impl_->mutex);
    if(impl_->stopped) return std::unexpected(SubmitError::stopped);
    const auto target=Impl::find(impl_->targets,id); const auto zone=Impl::find(impl_->zones,zone_id);
    if(!target || !zone) return std::unexpected(SubmitError::invalid);
    if(target->retiring) return std::unexpected(SubmitError::overloaded);
    target->zone=zone; ++impl_->revision; return {};
}
std::expected<void,SubmitError> ComputeRouter::enable_target(LoadTarget id,bool enabled) {
    std::lock_guard lock(impl_->mutex);
    if(impl_->stopped) return std::unexpected(SubmitError::stopped);
    const auto target=Impl::find(impl_->targets,id);
    if(!target) return std::unexpected(SubmitError::invalid);
    if(target->retiring) return std::unexpected(SubmitError::overloaded);
    target->enabled=enabled; ++impl_->revision; return {};
}
bool ComputeRouter::report_health(LoadTarget id,bool healthy) {
    std::lock_guard lock(impl_->mutex);
    if(impl_->stopped || !Impl::find(impl_->targets,id) || !impl_->balance.report_health(id,healthy)) return false;
    ++impl_->revision; return true;
}
bool ComputeRouter::report_load(LoadTarget id,ComputeObservation observation) {
    const auto now=Clock::now();
    if(observation.sampled_at==Clock::time_point{}) observation.sampled_at=now;
    if(observation.sampled_at>now || observation.valid_for<=Clock::duration::zero() ||
       (observation.cpu_utilization && (!std::isfinite(*observation.cpu_utilization) || *observation.cpu_utilization<0 || *observation.cpu_utilization>1))) return false;
    std::lock_guard lock(impl_->mutex);
    const auto target=Impl::find(impl_->targets,id);
    if(impl_->stopped || !target || target->retiring || (target->observation && observation.sampled_at<target->observation->sampled_at)) return false;
    target->observation=observation; ++impl_->revision; return true;
}
std::expected<ComputeLease,SubmitError> ComputeRouter::allocate(ComputeScope scope_id,ComputeRequest request,bool external) {
    auto state=std::make_shared<ComputeLease::State>(impl_);
    std::lock_guard lock(impl_->mutex);
    const auto reject=[&](SubmitError code) -> std::expected<ComputeLease,SubmitError> { ++impl_->rejected; return std::unexpected(code); };
    if(impl_->stopped) return reject(SubmitError::stopped);
    const auto scope=Impl::find(impl_->scopes,scope_id);
    const auto policy=request.work.policy.value_or(BalancePolicy::least_loaded);
    if(!scope || !request.work.units ||
       (request.work.priority!=Priority::high && request.work.priority!=Priority::normal && request.work.priority!=Priority::background) ||
       (policy!=BalancePolicy::least_loaded && policy!=BalancePolicy::least_outstanding && policy!=BalancePolicy::round_robin && policy!=BalancePolicy::flow_hash) ||
       (policy==BalancePolicy::flow_hash && !request.work.affinity) || (request.preferred_zone &&
       (!Impl::find(impl_->zones,*request.preferred_zone) || !impl_->covers(*scope,*request.preferred_zone)))) return reject(SubmitError::invalid);
    if(impl_->balance.stats().in_flight==impl_->options.capacity) return reject(SubmitError::full);
    if(!scope->options.enabled || !fits(scope->usage,request.work,scope->options.limits)) return reject(SubmitError::overloaded);
    impl_->refresh_locked();
    const auto now=Clock::now();
    bool preferred=false;
    long double best=0;
    impl_->candidates.clear();
    for(const auto& target:impl_->targets) if(target) {
        const auto& load=target->load; const auto& native=target->sample;
        if(!target->enabled || target->retiring || !target->zone->options.enabled || !impl_->covers(*scope,target->zone->id) ||
           !fits(target->zone->usage,request.work,target->zone->options.limits) || native.state!=RuntimeState::running ||
           !native.workers || std::max(native.pending,load.in_flight)>=native.capacity ||
           load.group!=request.work.group || load.draining || load.health!=TargetHealth::healthy ||
           (load.capabilities & request.work.capabilities)!=request.work.capabilities ||
           !fits(load.units,request.work.units,load.capacity) || !fits(load.memory_bytes,request.work.memory_bytes,load.memory_capacity)) continue;
        const bool local=request.preferred_zone && target->zone->id==*request.preferred_zone;
        if(request.preferred_zone && !request.cross_zone && !local) continue;
        const bool observed=fresh(target->observation,now);
        if(target->options.require_observation && !observed) continue;
        long double headroom=1;
        if(observed) {
            const auto& observation=*target->observation;
            if(observation.cpu_utilization) {
                if(*observation.cpu_utilization>=target->options.cpu_ceiling) continue;
                headroom*=std::max(0.01L,1.0L-static_cast<long double>(*observation.cpu_utilization));
            }
            if(!fits(observation.external_memory_bytes,load.memory_bytes) ||
               !fits(observation.external_memory_bytes+load.memory_bytes,request.work.memory_bytes,observation.memory_capacity)) continue;
            if(observation.memory_capacity) headroom*=std::max(0.01L,1.0L-
                (static_cast<long double>(observation.external_memory_bytes)+static_cast<long double>(load.memory_bytes))/
                static_cast<long double>(observation.memory_capacity));
        }
        const auto score=(static_cast<long double>(std::max(native.pending,load.units))+static_cast<long double>(request.work.units))/
            (static_cast<long double>(native.workers)*headroom);
        if(preferred && !local) continue;
        if((local && !preferred) || impl_->candidates.empty() || score<best) {
            impl_->candidates.clear(); best=score; preferred=local;
        }
        if(score==best) impl_->candidates.push_back(target->id);
    }
    if(impl_->candidates.empty()) return reject(SubmitError::overloaded);
    auto lease=impl_->balance.acquire(request.work,impl_->candidates);
    if(!lease) return reject(lease.error());
    state->target=Impl::find(impl_->targets,lease->target()); state->zone=state->target->zone; state->scope=scope;
    state->placement={lease->target(),state->zone->id,scope_id}; state->lease=std::move(*lease); state->resources=request.work;
    for(auto* usage:{&state->zone->usage,&state->scope->usage,&state->target->usage}) reserve(*usage,request.work);
    state->external=external; state->accounted=true;
    if(external) { ++impl_->external; ++state->target->external; }
    else ++impl_->internal;
    return ComputeLease(std::move(state));
}
std::expected<ComputeLease,SubmitError> ComputeRouter::acquire(ComputeScope scope,ComputeRequest request) { return allocate(scope,request,true); }
Submission ComputeRouter::post(Task task,Priority priority) { return submit(default_scope(),std::move(task),{.work={.priority=priority}}); }
Submission ComputeRouter::submit(ComputeScope scope,Task task,ComputeRequest request) {
    if(!task) { impl_->rejected_request(); return {{},SubmitError::invalid}; }
    auto lease=allocate(scope,request,false);
    if(!lease) return {{},lease.error()};
    const auto state=lease->state_;
    const auto submitted=impl_->work.post(*lease,[state,task=std::move(task)] { task(); state->complete(); },request.work.priority);
    if(!submitted) impl_->rejected_request();
    lease->state_.reset();
    return submitted;
}
std::expected<RoutedProcess,SubmitError> ComputeRouter::route_process(ComputeScope scope,process::Launch launch,
    core::Bytes input,process::CommunicationOptions communication,ComputeRequest request) {
    auto lease=allocate(scope,request,false);
    if(!lease) return std::unexpected(lease.error());
    const auto placement=lease->placement(); const auto state=lease->state_;
    auto native_id=std::make_shared<std::atomic<std::uint64_t>>(0);
    const auto deadline=communication.deadline;
    auto operation=impl_->processes.submit<process::Communication>(*lease,
        [state,native_id,launch=std::move(launch),input=std::move(input),communication=std::move(communication)](WorkContext& context) mutable -> core::Result<process::Communication> {
            std::stop_source stop;
            const auto request_stop=[&]{stop.request_stop();};
            std::stop_callback user(context.stop_token(),request_stop),owner(context.owner_stop_token(),request_stop),
                target_stop(state->lease.stop_token(),request_stop),external(communication.stop,request_stop);
            if(stop.stop_requested()) return core::fail(core::ErrorCode::cancelled,"Routed process cancelled before launch");
            struct Active final {
                ComputeLease::State& state;
                bool started=false;
                ~Active() { if(started) { std::lock_guard lock(state.owner->mutex); --state.target->active_processes; } }
            } active{*state};
            auto child=process::Process::start(launch);
            if(!child) { state->fail(child.error().cause); return std::unexpected(child.error()); }
            { std::lock_guard lock(state->owner->mutex); ++state->target->active_processes; active.started=true; }
            native_id->store(child->id());
            context.mark_effects(SideEffects::possible);
            communication.stop=stop.get_token(); communication.deadline=context.deadline();
            auto result=child->communicate(core::ByteView(input.data(),input.size()),communication);
            if(!result) { state->fail(result.error().cause); return std::unexpected(result.error()); }
            if(context.owner_stop_token().stop_requested() && result->exit.reason==process::ExitReason::cancelled)
                result->exit.reason=process::ExitReason::owner_removed;
            if(result->exit.reason==process::ExitReason::exited && !result->exit.code) state->complete();
            else if(result->exit.reason==process::ExitReason::exited || result->exit.reason==process::ExitReason::signaled) state->fail();
            return result;
        },{.deadline=deadline,.priority=request.work.priority});
    const auto admission=operation.admission();
    lease->state_.reset();
    if(admission!=SubmitError::none) { impl_->rejected_request(); return std::unexpected(admission); }
    return RoutedProcess(placement,std::move(operation),std::move(native_id));
}
ComputeSnapshot ComputeRouter::monitor() {
    std::lock_guard lock(impl_->mutex);
    impl_->refresh_locked();
    ComputeSnapshot snapshot;
    snapshot.sampled_at=Clock::now(); snapshot.revision=impl_->revision; snapshot.monitor_samples=impl_->samples;
    snapshot.rejected=impl_->rejected; snapshot.stopped=impl_->stopped; snapshot.load=impl_->balance.stats();
    for(const auto& zone:impl_->zones) if(zone) {
        AvailabilityZoneSnapshot value; value.zone=zone->id; value.options=zone->options; value.usage=zone->usage;
        value.overcommitted=overcommitted(zone->usage,zone->options.limits);
        for(const auto& target:impl_->targets) if(target && target->zone==zone && !target->retiring) ++value.targets;
        snapshot.zones.push_back(std::move(value));
    }
    for(const auto& scope:impl_->scopes) if(scope) snapshot.scopes.push_back({scope->id,scope->options,scope->usage,overcommitted(scope->usage,scope->options.limits)});
    for(const auto& target:impl_->targets) if(target) {
        ComputeTargetSnapshot value;
        value.target=target->id; value.zone=target->zone->id; value.load=target->load; value.runtime=target->sample;
        value.runtime_sampled_at=target->sampled_at; value.usage=target->usage; value.observation=target->observation;
        value.observation_fresh=fresh(target->observation,snapshot.sampled_at); value.enabled=target->enabled; value.retiring=target->retiring;
        value.active_callbacks=target->active_callbacks; value.active_processes=target->active_processes;
        value.callbacks_started=target->callbacks_started; value.callbacks_finished=target->callbacks_finished;
        value.queue_time=target->queue_time; value.execution_time=target->execution_time;
        snapshot.targets.push_back(std::move(value));
    }
    return snapshot;
}
Submission ComputeRouter::start_monitoring(EventLoop& loop,Clock::duration interval) {
    if(interval<=Clock::duration::zero()) return {{},SubmitError::invalid};
    std::unique_lock lifecycle(impl_->lifecycle,std::try_to_lock);
    if(!lifecycle.owns_lock()) return {{},SubmitError::overloaded};
    auto monitor=std::make_shared<Impl::Monitor>();
    std::lock_guard lock(impl_->mutex);
    if(impl_->stopped) return {{},SubmitError::stopped};
    if(impl_->monitoring) return {{},SubmitError::overloaded};
    const auto submitted=loop.every(interval,[owner=std::weak_ptr<Impl>(impl_),weak=std::weak_ptr<Impl::Monitor>(monitor)] {
        const auto state=owner.lock(); const auto active=weak.lock();
        if(state && active) (void)active->calls.run([&] { std::lock_guard guard(state->mutex); state->refresh_locked(); });
    });
    if(submitted) { monitor->token=submitted.token; impl_->monitoring=monitor; }
    return submitted;
}
ThreadResult ComputeRouter::stop_monitoring() {
    std::shared_ptr<Impl::Monitor> monitor;
    { std::lock_guard lock(impl_->mutex); monitor=std::exchange(impl_->monitoring,{}); }
    if(!monitor) return {};
    (void)monitor->token.cancel();
    return monitor->calls.close();
}
ThreadResult ComputeRouter::remove_target(LoadTarget id,ShutdownMode mode) {
    if(mode!=ShutdownMode::drain && mode!=ShutdownMode::cancel_pending) return error(ThreadErrorCode::invalid_work);
    if(Activity::contains(impl_.get())) return error(ThreadErrorCode::self_join);
    std::unique_lock lifecycle(impl_->lifecycle,std::try_to_lock);
    if(!lifecycle.owns_lock()) return error(ThreadErrorCode::busy);
    std::shared_ptr<Impl::Target> target;
    {
        std::lock_guard lock(impl_->mutex);
        target=Impl::find(impl_->targets,id);
        if(!target) return error(ThreadErrorCode::invalid_work);
        target->retiring=true; target->enabled=false; target->load.draining=true; ++impl_->revision;
        (void)impl_->balance.request_drain(id);
    }
    for(;;) {
        if(mode==ShutdownMode::drain) {
            std::unique_lock lock(impl_->mutex);
            if(target->external || (target->usage.in_flight && cannot_block())) return error(ThreadErrorCode::would_block);
            impl_->changed.wait(lock,[&]{return target->usage.in_flight==0;});
        }
        auto result=impl_->balance.remove_target(id,mode);
        if(result || result.error().code==ThreadErrorCode::work_failed) {
            std::lock_guard lock(impl_->mutex);
            for(auto& slot:impl_->targets) if(slot==target) { slot.reset(); break; }
            return result;
        }
        if(result.error().code!=ThreadErrorCode::would_block) return result;
        std::unique_lock lock(impl_->mutex);
        if(target->external || cannot_block()) return result;
        impl_->changed.wait_for(lock,std::chrono::milliseconds(1));
    }
}
ThreadResult ComputeRouter::close(ShutdownMode mode) {
    if(mode!=ShutdownMode::drain && mode!=ShutdownMode::cancel_pending) return error(ThreadErrorCode::invalid_work);
    if(Activity::contains(impl_.get())) return error(ThreadErrorCode::self_join);
    std::unique_lock lifecycle(impl_->lifecycle,std::try_to_lock);
    if(!lifecycle.owns_lock()) return error(ThreadErrorCode::busy);
    {
        std::lock_guard lock(impl_->mutex); impl_->stopped=true;
        for(const auto& target:impl_->targets) if(target) { target->enabled=false; target->retiring=true; target->load.draining=true; }
    }
    impl_->balance.request_stop();
    if(const auto stopped=stop_monitoring();!stopped) return stopped;
    if(mode==ShutdownMode::cancel_pending) { impl_->processes.request_close(); impl_->work.request_close(); }
    if(mode==ShutdownMode::drain) {
        std::unique_lock lock(impl_->mutex);
        if(impl_->external || (impl_->internal && cannot_block())) return error(ThreadErrorCode::would_block);
        impl_->changed.wait(lock,[&]{return impl_->internal==0;});
    }
    ThreadResult result;
    for(;;) {
        result=impl_->balance.close(mode);
        if(result || result.error().code==ThreadErrorCode::work_failed) break;
        if(result.error().code!=ThreadErrorCode::would_block) return result;
        std::unique_lock lock(impl_->mutex);
        if(impl_->external || cannot_block()) return result;
        impl_->changed.wait_for(lock,std::chrono::milliseconds(1));
    }
    if(const auto closed=impl_->processes.close();!closed) return closed;
    if(const auto closed=impl_->work.close();!closed) return closed;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->refresh_locked();
        for(const auto& target:impl_->targets) if(target) target->runtime=nullptr;
    }
    return result;
}
ComputeLease::~ComputeLease()=default;
ComputeLease::ComputeLease(ComputeLease&&) noexcept=default;
ComputeLease& ComputeLease::operator=(ComputeLease&&) noexcept=default;
ComputePlacement ComputeLease::placement() const noexcept { return state_ ? state_->placement : ComputePlacement{}; }
std::stop_token ComputeLease::stop_token() const noexcept { return state_ ? state_->lease.stop_token() : std::stop_token{}; }
Submission ComputeLease::post(Task task,Priority priority) const {
    if(!task) return {{},SubmitError::invalid};
    if(!state_) return {{},SubmitError::stopped};
    auto delivery=std::make_shared<State::Delivery>(state_,std::move(task));
    return state_->lease.post([delivery]{delivery->run();},priority);
}
void ComputeLease::complete() noexcept { if(state_) { state_->complete(); state_.reset(); } }
void ComputeLease::fail(std::exception_ptr cause) noexcept { if(state_) { state_->fail(std::move(cause)); state_.reset(); } }
void ComputeLease::cancel() noexcept { state_.reset(); }
} // namespace ii::runtime
