#include "LoadBalancer.h"
#include "TaskScope.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>
#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
#endif

namespace ii::runtime {
namespace {
struct Activity final {
    const void* owner;
    const void* target;
    Activity* previous;
    static thread_local Activity* current;
    Activity(const void* state,const void* selected):owner(state),target(selected),previous(current) { current=this; }
    ~Activity() { current=previous; }
    static bool contains(const void* state,const void* selected=nullptr) {
        for(auto* entry=current;entry;entry=entry->previous)
            if(entry->owner==state && (!selected || entry->target==selected)) return true;
        return false;
    }
};
thread_local Activity* Activity::current=nullptr;
ThreadResult error(ThreadErrorCode code,std::exception_ptr cause={}) {
    return std::unexpected(ThreadError{code,cause});
}
bool cannot_block() {
#if defined(__EMSCRIPTEN__)
    if(emscripten_is_main_browser_thread()) return true;
#endif
    return Runtime::is_worker_thread();
}
bool valid(ShutdownMode mode) { return mode==ShutdownMode::drain || mode==ShutdownMode::cancel_pending; }
bool valid(BalancePolicy policy) {
    return policy==BalancePolicy::round_robin || policy==BalancePolicy::least_loaded ||
        policy==BalancePolicy::least_outstanding || policy==BalancePolicy::flow_hash;
}
std::uint64_t mix(std::uint64_t value) {
    value=(value^(value>>30))*0xbf58476d1ce4e5b9ULL;
    value=(value^(value>>27))*0x94d049bb133111ebULL;
    return value^(value>>31);
}
LoadTarget next_target() {
    static std::atomic<std::uint64_t> next{1};
    auto value=next.load(std::memory_order_relaxed);
    do {
        if(value==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("LoadTarget identities exhausted");
    } while(!next.compare_exchange_weak(value,value+1,std::memory_order_relaxed));
    return {value};
}
}

struct LoadBalancer::Target final {
    LoadTargetOptions options;
    LoadTargetStats statistics;
    SubmitFunction submit;
    TaskScope scope;
    std::stop_source stop;
    std::uint32_t successes=0,failures=0;
    std::exception_ptr failure;
    Target(SubmitFunction execute,LoadTargetOptions settings):options(std::move(settings)),submit(std::move(execute)) {
        statistics.name=options.name;
        statistics.capacity=options.capacity;
        statistics.memory_capacity=options.memory_capacity;
        statistics.capabilities=options.capabilities;
        statistics.group=options.group;
        statistics.health=options.require_health_check ? TargetHealth::initial : TargetHealth::healthy;
    }
    Submission post(Task task,Priority priority) { return submit(std::move(task),priority); }
};

struct LoadBalancer::Impl final {
    const LoadBalancerOptions options;
    mutable std::mutex mutex;
    std::mutex lifecycle;
    std::condition_variable changed;
    std::vector<std::shared_ptr<Target>> targets;
    struct Group final { LoadGroup id=0; std::size_t cursor=0,targets=0; };
    std::vector<Group> groups;
    LoadBalancerStats statistics;
    std::exception_ptr failure;
    explicit Impl(LoadBalancerOptions settings):options(settings),targets(settings.max_targets),groups(settings.max_targets) {
        statistics.capacity=options.capacity;
        statistics.max_targets=options.max_targets;
    }
    std::shared_ptr<Target> find(LoadTarget id) const {
        for(const auto& target:targets) if(target && target->statistics.target==id) return target;
        return {};
    }
    ThreadResult result(const std::shared_ptr<Target>& target={}) const {
        const auto failed=target ? target->failure : failure;
        const auto count=target ? target->statistics.failed : statistics.failed;
        return count ? error(ThreadErrorCode::work_failed,failed) : ThreadResult{};
    }
    ThreadResult wait(const std::shared_ptr<Target>& target={},bool lifecycle=false) {
        if(Activity::contains(this,target.get())) return error(ThreadErrorCode::self_join);
        std::unique_lock lock(mutex);
        const auto idle=[&]{return target ? target->statistics.in_flight==0 : statistics.in_flight==0;};
        // External reservations can be held by the lifecycle caller itself.
        // Request cancellation, then let that owner release and retry the barrier.
        if(lifecycle && (target ? target->statistics.reservations : statistics.reservations))
            return error(ThreadErrorCode::would_block);
        if(!idle() && cannot_block()) return error(ThreadErrorCode::would_block);
        changed.wait(lock,idle);
        return result(target);
    }
};

struct LoadBalancer::Ticket final {
    enum class Outcome { cancelled, completed, failed };
    std::shared_ptr<Impl> owner;
    std::shared_ptr<Target> target;
    Task task;
    LoadRequest request;
    std::atomic<Outcome> outcome{Outcome::cancelled};
    bool admitted=false;
    bool reservation=false;
    std::exception_ptr failure;
    Ticket(std::shared_ptr<Impl> state,Task work,LoadRequest resources)
        :owner(std::move(state)),task(std::move(work)),request(resources) {}
    ~Ticket() {
        Activity retiring(owner.get(),target.get());
        task=nullptr; // resource destructors run without a lock, before quota returns
        if(!target) return;
        std::lock_guard lock(owner->mutex);
        auto& all=owner->statistics;
        auto& selected=target->statistics;
        --all.in_flight; --selected.in_flight;
        if(reservation) { --all.reservations; --selected.reservations; }
        selected.units-=request.units; selected.memory_bytes-=request.memory_bytes;
        if(admitted) {
            switch(outcome.load(std::memory_order_acquire)) {
                case Outcome::completed: ++all.completed; ++selected.completed; break;
                case Outcome::failed:
                    ++all.failed; ++selected.failed;
                    if(!owner->failure) owner->failure=failure;
                    if(!target->failure) target->failure=failure;
                    break;
                case Outcome::cancelled: ++all.cancelled; ++selected.cancelled; break;
            }
        } else { ++all.rejected; ++selected.rejected; }
        owner->changed.notify_all();
    }
    void admit() {
        std::lock_guard lock(owner->mutex);
        if(!admitted) {
            admitted=true;
            ++owner->statistics.submitted; ++target->statistics.submitted;
        }
    }
    void run() {
        Activity executing(owner.get(),target.get());
        admit(); // an inline executor may finish or throw before post returns
        try { task(); outcome.store(Outcome::completed,std::memory_order_release); }
        catch(...) {
            fail(std::current_exception());
            throw;
        }
    }
    void fail(std::exception_ptr cause) noexcept {
        std::lock_guard lock(owner->mutex);
        if(!failure) failure=std::move(cause);
        outcome.store(Outcome::failed,std::memory_order_release);
    }
    struct Delivery final {
        std::shared_ptr<Ticket> ticket;
        Task task;
        Delivery(std::shared_ptr<Ticket> request,Task work):ticket(std::move(request)),task(std::move(work)) {}
        ~Delivery() {
            Activity retiring(ticket->owner.get(),ticket->target.get());
            task=nullptr;
        }
        void run() {
            Activity active(ticket->owner.get(),ticket->target.get());
            try { task(); }
            catch(...) { ticket->fail(std::current_exception()); throw; }
        }
    };
};

LoadBalancer::LoadBalancer(LoadBalancerOptions options) {
    if(!options.capacity || !options.max_targets || !valid(options.policy))
        throw std::invalid_argument("LoadBalancer requires positive bounds and a valid policy");
    impl_=std::make_shared<Impl>(options);
}
LoadBalancer::~LoadBalancer() {
    const auto closed=close();
    if(!closed && closed.error().code!=ThreadErrorCode::work_failed) std::terminate();
}
std::expected<LoadTarget,SubmitError> LoadBalancer::add_target_erased(SubmitFunction submit,LoadTargetOptions options) {
    if(!options.capacity || !options.unhealthy_threshold || !options.healthy_threshold)
        return std::unexpected(SubmitError::invalid);
    auto target=std::make_shared<Target>(std::move(submit),std::move(options));
    std::lock_guard lock(impl_->mutex);
    if(impl_->statistics.stopped) return std::unexpected(SubmitError::stopped);
    const auto available=std::find(impl_->targets.begin(),impl_->targets.end(),nullptr);
    if(available==impl_->targets.end()) return std::unexpected(SubmitError::full);
    auto group=std::find_if(impl_->groups.begin(),impl_->groups.end(),[&](const auto& entry) {
        return entry.targets && entry.id==target->options.group;
    });
    if(group==impl_->groups.end()) {
        group=std::find_if(impl_->groups.begin(),impl_->groups.end(),[](const auto& entry){return !entry.targets;});
        *group={.id=target->options.group};
    }
    target->statistics.target=next_target();
    *available=target;
    ++group->targets;
    ++impl_->statistics.targets;
    return target->statistics.target;
}
Submission LoadBalancer::post(Task task,Priority priority) {
    return submit(std::move(task),{.priority=priority});
}
Submission LoadBalancer::submit(Task task,LoadRequest request) {
    auto prepared=prepare(std::move(task),request,false);
    if(!prepared) return {{},prepared.error()};
    auto ticket=std::move(*prepared);
    // Admission may synchronously execute/reject/destroy a callback. Do not hold
    // the dispatcher lock and never replay an operation on another executor.
    const auto submitted=ticket->target->scope.post(*ticket->target,[ticket]{ticket->run();},request.priority);
    if(submitted) ticket->admit();
    return submitted;
}
std::expected<std::shared_ptr<LoadBalancer::Ticket>,SubmitError>
LoadBalancer::prepare(Task task,LoadRequest request,bool reservation,std::span<const LoadTarget> candidates,bool restricted) {
    auto ticket=std::make_shared<Ticket>(impl_,std::move(task),request);
    {
        std::lock_guard lock(impl_->mutex);
        auto reject=[&](SubmitError reason) -> std::expected<std::shared_ptr<Ticket>,SubmitError> {
            ++impl_->statistics.rejected; return std::unexpected(reason);
        };
        const auto policy=request.policy.value_or(impl_->options.policy);
        if((!reservation && !ticket->task) || !request.units || !valid(policy) ||
            (policy==BalancePolicy::flow_hash && !request.affinity) || (request.priority!=Priority::high &&
            request.priority!=Priority::normal && request.priority!=Priority::background)) return reject(SubmitError::invalid);
        if(impl_->statistics.stopped) return reject(SubmitError::stopped);
        if(impl_->statistics.in_flight==impl_->options.capacity) return reject(SubmitError::full);
        auto group=std::find_if(impl_->groups.begin(),impl_->groups.end(),[&](const auto& entry) {
            return entry.targets && entry.id==request.group;
        });
        if(group==impl_->groups.end()) return reject(SubmitError::overloaded);
        std::size_t chosen=impl_->targets.size();
        long double best=0;
        for(std::size_t i=0;i<impl_->targets.size();++i) {
            const auto index=(group->cursor+i)%impl_->targets.size();
            const auto& target=impl_->targets[index];
            if(!target) continue;
            const auto& load=target->statistics;
            if(restricted && std::find(candidates.begin(),candidates.end(),load.target)==candidates.end()) continue;
            if(load.group!=request.group || load.draining || load.health!=TargetHealth::healthy ||
                (load.capabilities & request.capabilities)!=request.capabilities ||
                request.units>load.capacity-load.units ||
                request.memory_bytes>std::numeric_limits<std::size_t>::max()-load.memory_bytes ||
                (load.memory_capacity && request.memory_bytes>load.memory_capacity-load.memory_bytes)) continue;
            auto projected=static_cast<long double>(load.units+request.units)/static_cast<long double>(load.capacity);
            if(policy==BalancePolicy::least_outstanding) projected=static_cast<long double>(load.in_flight);
            if(policy==BalancePolicy::flow_hash) {
                // Weighted rendezvous: a topology change only remaps keys whose
                // winning target changed. Existing connections retain a lease.
                const auto bits=mix(*request.affinity^mix(load.target.id));
                const auto uniform=(static_cast<long double>(bits>>11)+1.0L)/9007199254740993.0L;
                projected=-std::log(uniform)/static_cast<long double>(load.capacity);
            }
            if(chosen==impl_->targets.size() || projected<best ||
               (policy==BalancePolicy::flow_hash && projected==best && load.target.id<impl_->targets[chosen]->statistics.target.id)) {
                chosen=index; best=projected;
            }
            if(policy==BalancePolicy::round_robin) break;
        }
        if(chosen==impl_->targets.size()) return reject(SubmitError::overloaded);
        group->cursor=(chosen+1)%impl_->targets.size();
        ticket->target=impl_->targets[chosen];
        auto& selected=ticket->target->statistics;
        ++selected.in_flight; selected.units+=request.units; selected.memory_bytes+=request.memory_bytes;
        ++impl_->statistics.in_flight;
        ticket->reservation=reservation;
        if(reservation) { ++selected.reservations; ++impl_->statistics.reservations; }
        impl_->statistics.high_watermark=std::max(impl_->statistics.high_watermark,impl_->statistics.in_flight);
    }
    return ticket;
}
std::expected<LoadLease,SubmitError> LoadBalancer::acquire(LoadRequest request) {
    auto prepared=prepare({},request,true);
    if(!prepared) return std::unexpected(prepared.error());
    (*prepared)->admit();
    return LoadLease(std::move(*prepared));
}
std::expected<LoadLease,SubmitError> LoadBalancer::acquire(LoadRequest request,std::span<const LoadTarget> candidates) {
    auto prepared=prepare({},request,true,candidates,true);
    if(!prepared) return std::unexpected(prepared.error());
    (*prepared)->admit();
    return LoadLease(std::move(*prepared));
}
bool LoadBalancer::report_health(LoadTarget id,bool healthy) {
    std::lock_guard lock(impl_->mutex);
    const auto target=impl_->find(id);
    if(!target || target->statistics.draining) return false;
    if(healthy) {
        target->failures=0;
        if(target->successes<target->options.healthy_threshold) ++target->successes;
        if(target->successes==target->options.healthy_threshold) target->statistics.health=TargetHealth::healthy;
    } else {
        target->successes=0;
        if(target->failures<target->options.unhealthy_threshold) ++target->failures;
        if(target->failures==target->options.unhealthy_threshold) target->statistics.health=TargetHealth::unhealthy;
    }
    return true;
}
bool LoadBalancer::request_drain(LoadTarget id) {
    std::lock_guard lock(impl_->mutex);
    const auto target=impl_->find(id);
    if(!target) return false;
    target->statistics.draining=true;
    return true;
}
ThreadResult LoadBalancer::remove_target(LoadTarget id,ShutdownMode mode) {
    if(!valid(mode)) return error(ThreadErrorCode::invalid_work);
    if(Activity::contains(impl_.get())) return error(ThreadErrorCode::self_join);
    std::unique_lock lifecycle(impl_->lifecycle,std::try_to_lock);
    if(!lifecycle.owns_lock()) return error(ThreadErrorCode::busy);
    std::shared_ptr<Target> target;
    {
        std::lock_guard lock(impl_->mutex);
        target=impl_->find(id);
        if(!target) return error(ThreadErrorCode::invalid_work);
        target->statistics.draining=true;
    }
    if(mode==ShutdownMode::drain) {
        const auto idle=impl_->wait(target,true);
        if(!idle && idle.error().code!=ThreadErrorCode::work_failed) return idle;
    }
    if(mode==ShutdownMode::cancel_pending) target->stop.request_stop();
    const auto closed=target->scope.close();
    if(!closed) return closed;
    const auto idle=impl_->wait(target,true);
    if(!idle && idle.error().code!=ThreadErrorCode::work_failed) return idle;
    {
        std::lock_guard lock(impl_->mutex);
        for(auto& slot:impl_->targets) if(slot==target) { slot.reset(); --impl_->statistics.targets; break; }
        for(auto& group:impl_->groups) if(group.targets && group.id==target->options.group) { --group.targets; break; }
    }
    return idle;
}
void LoadBalancer::request_stop() {
    std::lock_guard lock(impl_->mutex);
    impl_->statistics.stopped=true;
    for(const auto& target:impl_->targets) if(target) target->statistics.draining=true;
}
ThreadResult LoadBalancer::wait_idle() const { return impl_->wait(); }
ThreadResult LoadBalancer::close(ShutdownMode mode) {
    if(!valid(mode)) return error(ThreadErrorCode::invalid_work);
    if(Activity::contains(impl_.get())) return error(ThreadErrorCode::self_join);
    std::unique_lock lifecycle(impl_->lifecycle,std::try_to_lock);
    if(!lifecycle.owns_lock()) return error(ThreadErrorCode::busy);
    request_stop();
    if(mode==ShutdownMode::cancel_pending) {
        for(std::size_t i=0;i<impl_->targets.size();++i) {
            std::shared_ptr<Target> target;
            { std::lock_guard lock(impl_->mutex); target=impl_->targets[i]; }
            if(target) { target->stop.request_stop(); target->scope.request_close(); }
        }
    }
    if(mode==ShutdownMode::drain) {
        const auto idle=impl_->wait({},true);
        if(!idle && idle.error().code!=ThreadErrorCode::work_failed) return idle;
    }
    // stopped prevents registration and lifecycle serializes removal. Only
    // copies of target handles are taken under the lock; cleanup runs outside it.
    for(std::size_t i=0;i<impl_->targets.size();++i) {
        std::shared_ptr<Target> target;
        { std::lock_guard lock(impl_->mutex); target=impl_->targets[i]; }
        if(target) {
            const auto closed=target->scope.close();
            if(!closed) return closed;
        }
    }
    const auto idle=impl_->wait({},true);
    if(!idle && idle.error().code!=ThreadErrorCode::work_failed) return idle;
    for(auto& slot:impl_->targets) {
        std::shared_ptr<Target> retired;
        { std::lock_guard lock(impl_->mutex); retired.swap(slot); }
    }
    {
        std::lock_guard lock(impl_->mutex);
        impl_->statistics.targets=0;
        for(auto& group:impl_->groups) group.targets=0;
    }
    return idle;
}
LoadBalancerStats LoadBalancer::stats() const {
    std::lock_guard lock(impl_->mutex);
    auto result=impl_->statistics;
    for(const auto& target:impl_->targets) if(target) {
        if(target->statistics.draining) ++result.draining_targets;
        else if(target->statistics.health==TargetHealth::healthy) ++result.healthy_targets;
    }
    return result;
}
std::optional<LoadTargetStats> LoadBalancer::target_stats(LoadTarget id) const {
    std::lock_guard lock(impl_->mutex);
    const auto target=impl_->find(id);
    return target ? std::optional(target->statistics) : std::nullopt;
}
LoadLease::~LoadLease()=default;
LoadLease::LoadLease(LoadLease&&) noexcept=default;
LoadLease& LoadLease::operator=(LoadLease&&) noexcept=default;
LoadTarget LoadLease::target() const noexcept { return ticket_ ? ticket_->target->statistics.target : LoadTarget{}; }
std::stop_token LoadLease::stop_token() const noexcept { return ticket_ ? ticket_->target->stop.get_token() : std::stop_token{}; }
Submission LoadLease::post(Task task,Priority priority) const {
    if(!task || (priority!=Priority::high && priority!=Priority::normal && priority!=Priority::background))
        return {{},SubmitError::invalid};
    if(!ticket_) return {{},SubmitError::stopped};
    auto delivery=std::make_shared<LoadBalancer::Ticket::Delivery>(ticket_,std::move(task));
    return ticket_->target->scope.post(*ticket_->target,[delivery]{delivery->run();},priority);
}
void LoadLease::complete() noexcept {
    if(ticket_) {
        auto expected=LoadBalancer::Ticket::Outcome::cancelled;
        (void)ticket_->outcome.compare_exchange_strong(expected,LoadBalancer::Ticket::Outcome::completed);
        ticket_.reset();
    }
}
void LoadLease::fail(std::exception_ptr cause) noexcept {
    if(ticket_) { ticket_->fail(std::move(cause)); ticket_.reset(); }
}
void LoadLease::cancel() noexcept { ticket_.reset(); }
} // namespace ii::runtime
