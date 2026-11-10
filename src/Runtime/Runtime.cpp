#include "Runtime.h"
#include "HostLoop.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
#endif

namespace ii::runtime {
namespace {
constexpr auto none = std::numeric_limits<std::size_t>::max();
constexpr std::array wheel{Priority::high, Priority::high, Priority::high,
    Priority::high, Priority::normal, Priority::normal, Priority::background};
thread_local const void* current_runtime = nullptr;
thread_local const void* current_lane = nullptr;
struct Retirement {
    const void* runtime;
    const void* lane;
    Retirement* previous;
    static thread_local Retirement* current;
    Retirement(const void* owner,const void* value):runtime(owner),lane(value),previous(current){current=this;}
    ~Retirement(){current=previous;}
    static bool contains(const void* value) {
        for(auto* entry=current;entry;entry=entry->previous) if(value && entry->lane==value) return true;
        return false;
    }
    static bool contains_runtime(const void* value) {
        for(auto* entry=current;entry;entry=entry->previous) if(entry->runtime==value) return true;
        return false;
    }
};
thread_local Retirement* Retirement::current=nullptr;
bool host_thread() {
#if defined(__EMSCRIPTEN__)
    return emscripten_is_main_browser_thread();
#else
    return false;
#endif
}
ThreadResult failure(ThreadErrorCode code, std::exception_ptr cause = {}) {
    return std::unexpected(ThreadError{code, cause});
}
}

struct Runtime::Lane {
    std::size_t head = none, tail = none;
    bool active = false;
    bool closed = false;
    std::size_t pending = 0;
    std::condition_variable idle;
};

struct Runtime::StatsSubscription final { Task wake; std::atomic<bool> active{true}; };

struct Runtime::Shared {
    struct WorkerControl final { std::atomic<bool> enabled{false},retire{false}; };
    struct RetirementWait final { std::condition_variable changed; std::size_t count=0; };
    enum class Status { free, queued, executing, retiring };
    struct Slot {
        Task task;
        std::shared_ptr<Lane> lane;
        std::size_t previous = none, next = none;
        std::size_t queue = 0, ready_position = none;
        std::uint64_t sequence = 0, generation = 0;
        Priority priority = Priority::normal;
        Status status = Status::free;
    };
    struct Queue final {
        RuntimeQueueStats statistics;
        std::array<std::vector<std::size_t>,3> ready;
    };
    mutable std::mutex mutex;
    std::condition_variable changed, idle;
    std::vector<Slot> slots;
    std::unordered_map<std::size_t,std::unique_ptr<RetirementWait>> retirement_waiters;
    std::vector<std::size_t> free;
    std::vector<Queue> queues;
    const std::uint64_t owner = TaskToken::next_owner();
    std::uint64_t sequence = 0, generation = 0;
    std::size_t cursor = 0;
    RuntimeStats statistics;
    bool admission_paused = false;
    std::atomic<std::uint64_t> stats_version{1};
    std::exception_ptr first_failure;
    using Observers=std::vector<std::weak_ptr<StatsSubscription>>;
    std::mutex observers_mutex;
    std::shared_ptr<const Observers> observers;
    std::atomic<bool> has_observers{false};

    // Called under mutex; saturation makes readers conservatively resample.
    void touch_stats() noexcept {
        const auto value=stats_version.load(std::memory_order_relaxed);
        if(value!=(std::numeric_limits<std::uint64_t>::max)())
            stats_version.store(value+1,std::memory_order_release);
    }

    void notify_stats() noexcept {
        if (!has_observers.load(std::memory_order_acquire)) return;
        const auto snapshot=std::atomic_load_explicit(&observers,std::memory_order_acquire);
        if (!snapshot) return;
        for (const auto& weak:*snapshot) if (const auto observer=weak.lock(); observer && observer->active.load(std::memory_order_acquire)) {
            try { observer->wake(); } catch (...) {}
        }
    }

    Shared(std::size_t capacity,std::size_t queue_count) : queues(queue_count) {
        if (!capacity) throw std::invalid_argument("Runtime capacity must be positive");
        if (!queue_count || queue_count>capacity) throw std::invalid_argument("Runtime queue count exceeds capacity");
        for(std::size_t i=0;i<queue_count;++i) {
            auto& queue=queues[i];
            queue.statistics.index=i;
            queue.statistics.capacity=capacity/queue_count+static_cast<std::size_t>(i<capacity%queue_count);
        }
        statistics.capacity = capacity;
        statistics.queues = queue_count;
    }
    // Called under mutex, before starting workers. Build storage separately so
    // allocation failure leaves the dormant runtime unchanged.
    void allocate_slots() {
        if (!slots.empty()) return;
        const auto capacity=statistics.capacity;
        std::vector<Slot> next_slots(capacity);
        std::vector<std::size_t> next_free;
        next_free.reserve(capacity);
        std::vector<std::array<std::vector<std::size_t>,3>> next_ready(queues.size());
        for(std::size_t i=0;i<queues.size();++i)
            for(auto& heap:next_ready[i]) heap.reserve(queues[i].statistics.capacity);
        for(std::size_t i=capacity;i;--i) {
            next_slots[i-1].queue=(i-1)%queues.size();
            next_free.push_back(i-1);
        }
        slots.swap(next_slots);
        free.swap(next_free);
        for(std::size_t i=0;i<queues.size();++i) queues[i].ready.swap(next_ready[i]);
    }
    // All jobs and their captures have retired before storage is returned.
    void release_slots() noexcept {
        std::vector<Slot>().swap(slots);
        std::vector<std::size_t>().swap(free);
        for(auto& queue:queues)
            for(auto& heap:queue.ready) std::vector<std::size_t>().swap(heap);
    }
    void ready_swap(std::vector<std::size_t>& heap,std::size_t left,std::size_t right) noexcept {
        std::swap(heap[left],heap[right]);
        slots[heap[left]].ready_position=left;
        slots[heap[right]].ready_position=right;
    }
    bool older(std::size_t left,std::size_t right) const noexcept {
        return slots[left].sequence<slots[right].sequence;
    }
    void sift_up(std::vector<std::size_t>& heap,std::size_t position) noexcept {
        while(position) {
            const auto parent=(position-1)/2;
            if(!older(heap[position],heap[parent])) break;
            ready_swap(heap,position,parent); position=parent;
        }
    }
    void sift_down(std::vector<std::size_t>& heap,std::size_t position) noexcept {
        while(position<heap.size()/2) {
            auto child=position*2+1;
            if(child+1<heap.size() && older(heap[child+1],heap[child])) ++child;
            if(!older(heap[child],heap[position])) break;
            ready_swap(heap,position,child); position=child;
        }
    }
    void ready_add(std::size_t index) {
        auto& slot=slots[index];
        auto& queue=queues[slot.queue];
        auto& heap=queue.ready[static_cast<std::size_t>(slot.priority)];
        slot.ready_position=heap.size();
        heap.push_back(index); // Each shard owns no more slots than its reserved heap capacity.
        ++queue.statistics.ready;
        sift_up(heap,slot.ready_position);
    }
    void ready_remove(std::size_t index) noexcept {
        auto& slot=slots[index];
        if(slot.ready_position==none) return;
        auto& queue=queues[slot.queue];
        auto& heap=queue.ready[static_cast<std::size_t>(slot.priority)];
        const auto position=slot.ready_position;
        ready_swap(heap,position,heap.size()-1);
        heap.pop_back();
        slot.ready_position=none;
        --queue.statistics.ready;
        if(position==heap.size()) return;
        if(position && older(heap[position],heap[(position-1)/2])) sift_up(heap,position);
        else sift_down(heap,position);
    }
    void unlink(std::size_t index) {
        auto& slot = slots[index];
        if (slot.lane) {
            if (slot.previous == none) slot.lane->head = slot.next;
            else slots[slot.previous].next = slot.next;
            if (slot.next == none) slot.lane->tail = slot.previous;
            else slots[slot.next].previous = slot.previous;
        }
        slot.previous = slot.next = none;
    }
    void release(std::size_t index) {
        auto& slot = slots[index];
        if (slot.lane && --slot.lane->pending==0 && slot.lane->closed) slot.lane->idle.notify_all();
        --queues[slot.queue].statistics.pending;
        slot.lane.reset();
        slot.status = Status::free;
        slot.previous=slot.next=none; slot.sequence=0; slot.priority=Priority::normal;
        free.push_back(index);
        --statistics.pending;
        touch_stats();
        if (!retirement_waiters.empty()) {
            const auto waiter=retirement_waiters.find(index);
            if (waiter!=retirement_waiters.end()) waiter->second->changed.notify_all();
        }
        if (!statistics.pending) idle.notify_all();
    }
    std::size_t select() {
        for (std::size_t attempt = 0; attempt < wheel.size(); ++attempt) {
            const auto priority = wheel[cursor];
            cursor = (cursor + 1) % wheel.size();
            std::size_t chosen = none;
            for (const auto& queue:queues) {
                const auto& heap=queue.ready[static_cast<std::size_t>(priority)];
                if(!heap.empty() && (chosen==none || older(heap.front(),chosen))) chosen=heap.front();
            }
            if (chosen != none) { ready_remove(chosen); return chosen; }
        }
        return none;
    }
    ThreadResult result() const {
        if (first_failure) return failure(ThreadErrorCode::work_failed, first_failure);
        return {};
    }
    void run(std::size_t home_queue,const std::shared_ptr<WorkerControl>& control) {
        current_runtime = this;
        for (;;) {
            std::size_t index = none;
            Task task;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] {
                    if (control->retire.load(std::memory_order_acquire)) return true;
                    if (statistics.state == RuntimeState::cancelling || statistics.state == RuntimeState::stopped)
                        return true;
                    if (statistics.state == RuntimeState::draining && !statistics.pending) return true;
                    if (!control->enabled.load(std::memory_order_acquire)) return false;
                    index = select();
                    return index != none;
                });
                if (index == none) break;
                auto& slot = slots[index];
                unlink(index);
                if (slot.lane) slot.lane->active = true;
                auto& queue=queues[slot.queue].statistics;
                ++queue.active;
                if(slot.queue!=home_queue) ++queue.cross_queue;
                current_lane = slot.lane.get();
                slot.status = Status::executing;
                ++statistics.active;
                touch_stats();
                task.swap(slot.task);
            }
            notify_stats();
            std::exception_ptr error;
            try { task(); } catch (...) { error = std::current_exception(); }
            // Captures may reenter submission; never destroy them under the lock.
            task = nullptr;
            current_lane = nullptr;
            bool lane_ready=false,drained=false;
            {
                std::lock_guard lock(mutex);
                auto& slot = slots[index];
                if (slot.lane) {
                    slot.lane->active = false;
                    if(slot.lane->head!=none) { ready_add(slot.lane->head); lane_ready=true; }
                }
                auto& queue=queues[slot.queue].statistics;
                --queue.active; ++queue.executed;
                if (error) {
                    ++statistics.failed;
                    if (!first_failure) first_failure = error;
                }
                ++statistics.executed;
                --statistics.active;
                release(index);
                drained=statistics.state==RuntimeState::draining && !statistics.pending;
            }
            if (drained) changed.notify_all();
            else if (lane_ready) changed.notify_one();
            notify_stats();
        }
        current_runtime = nullptr;
    }
};

struct Runtime::Impl {
    struct Worker final { std::unique_ptr<Thread> thread; std::shared_ptr<Shared::WorkerControl> control; };
    std::shared_ptr<Shared> shared;
    std::vector<Worker> workers;
    std::mutex lifecycle;
    std::mutex sessions_mutex;
    std::vector<std::weak_ptr<Session::State>> sessions;
    ObjectOwner objects;
    bool joining = false, resizing = false, workers_joined = false, joined = false;
    std::optional<ThreadError> worker_failure;
    explicit Impl(RuntimeOptions options) {
        if(!options.capacity) throw std::invalid_argument("Runtime capacity must be positive");
        if(options.worker_policy!=RuntimeWorkerPolicy::none && options.worker_policy!=RuntimeWorkerPolicy::bounded && options.worker_policy!=RuntimeWorkerPolicy::hardware)
            throw std::invalid_argument("invalid Runtime worker policy");
        auto count=options.workers;
        if(!count && options.worker_policy!=RuntimeWorkerPolicy::none) {
            const auto hint=static_cast<std::size_t>(std::thread::hardware_concurrency());
            count=options.worker_policy==RuntimeWorkerPolicy::hardware
                ? std::min(options.capacity,std::max(std::size_t{1},hint))
                : std::min(options.capacity,std::clamp(hint,std::size_t{2},std::size_t{4}));
        }
        if(count && !Thread::supported()) throw std::runtime_error("Runtime requires native or WASM pthread workers");
        const auto queue_count=options.queues ? options.queues : std::max(std::size_t{1},std::min(count,options.capacity));
        if(queue_count>options.capacity) throw std::invalid_argument("Runtime queue count exceeds capacity");
        shared=std::make_shared<Shared>(options.capacity,queue_count);
        if(count) shared->allocate_slots();
        else shared->admission_paused=true;
        shared->statistics.workers = count;
        workers.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto control=std::make_shared<Shared::WorkerControl>();control->enabled.store(true);
            workers.push_back({std::make_unique<Thread>(),std::move(control)});
        }
        try {
            for (std::size_t i=0;i<workers.size();++i) {
                auto started = workers[i].thread->start([state = shared,control=workers[i].control,home=i%queue_count](ThreadContext&) { state->run(home,control); });
                if (!started) {
                    if (started.error().cause) std::rethrow_exception(started.error().cause);
                    throw std::runtime_error("Runtime worker startup rejected; use an application worker on WASM");
                }
            }
        } catch (...) {
            {
                std::lock_guard lock(shared->mutex);
                shared->statistics.state = RuntimeState::cancelling;
            }
            shared->changed.notify_all();
            for (auto& owned : workers) (void)owned.thread->join();
            throw;
        }
    }
};

Runtime::Runtime(RuntimeOptions options) : impl_(std::make_unique<Impl>(options)) {}
Runtime::~Runtime() {
    auto result = join();
    if (!result && result.error().code != ThreadErrorCode::work_failed) std::terminate();
}
Submission Runtime::submit(const std::shared_ptr<Shared>& shared, const std::shared_ptr<Lane>& lane,
                           Task task, Priority priority) {
    if (!shared) return {{}, SubmitError::stopped};
    std::unique_lock lock(shared->mutex);
    auto reject = [&](SubmitError error) {
        ++shared->statistics.rejected;
        shared->touch_stats();
        lock.unlock(); shared->notify_stats();
        return Submission{{}, error};
    };
    if (!task || (priority != Priority::high && priority != Priority::normal && priority != Priority::background))
        return reject(SubmitError::invalid);
    if (shared->statistics.state != RuntimeState::running) return reject(SubmitError::stopped);
    if (shared->admission_paused) return reject(SubmitError::overloaded);
    if (lane && lane->closed) return reject(SubmitError::stopped);
    if (shared->free.empty()) return reject(SubmitError::full);
    const auto index = shared->free.back();
    shared->free.pop_back();
    auto& slot = shared->slots[index];
    slot.task.swap(task);
    slot.lane = lane;
    slot.priority = priority;
    slot.sequence = ++shared->sequence;
    slot.generation=++shared->generation;
    slot.status = Shared::Status::queued;
    if (lane) {
        ++lane->pending;
        slot.previous = lane->tail;
        if (lane->tail != none) shared->slots[lane->tail].next = index;
        else lane->head = index;
        lane->tail = index;
    }
    auto& queue=shared->queues[slot.queue].statistics;
    ++queue.pending; ++queue.submitted;
    const bool ready=!lane || (!lane->active && lane->head==index);
    if(ready) shared->ready_add(index);
    ++shared->statistics.submitted;
    ++shared->statistics.pending;
    shared->statistics.high_watermark = std::max(shared->statistics.high_watermark, shared->statistics.pending);
    shared->touch_stats();
    TaskToken token;
    token.owner_ = shared->owner;
    token.slot_ = index;
    token.generation_ = slot.generation;
    token.executor_=shared;
    token.cancel_=&Runtime::cancel_shared;
    token.pending_=&Runtime::pending_shared;
    token.wait_=&Runtime::wait_shared;
    lock.unlock();
    if (ready) shared->changed.notify_one();
    shared->notify_stats();
    return {token, SubmitError::none};
}
Submission Runtime::post(Task task, Priority priority) { return submit(impl_->shared, {}, std::move(task), priority); }
ThreadResult Runtime::resize_workers(std::size_t count) {
    const auto state=impl_->shared;
    if(current_runtime==state.get() || Retirement::contains_runtime(state.get())) return failure(ThreadErrorCode::self_join);
    std::size_t previous=0;
    {
        std::unique_lock lifecycle(impl_->lifecycle);
        if(impl_->joining || impl_->resizing || impl_->joined) return failure(ThreadErrorCode::busy);
        std::lock_guard lock(state->mutex);
        if(state->statistics.state!=RuntimeState::running) return failure(ThreadErrorCode::invalid_work);
        previous=impl_->workers.size();
        if(count==previous) return {};
        if(count && !Thread::supported()) return failure(ThreadErrorCode::unsupported);
        if(host_thread() || HostLoop::is_current_thread()) return failure(ThreadErrorCode::would_block);
        if(count>state->statistics.capacity) return failure(ThreadErrorCode::invalid_work);
        try {
            if(count>previous) {
                impl_->workers.reserve(count);
                state->allocate_slots();
            }
        }
        catch(...) { return failure(ThreadErrorCode::start_failed,std::current_exception()); }
        impl_->resizing=true;
    }
    struct ResizeGuard final {
        Impl& impl;
        ~ResizeGuard(){std::lock_guard lock(impl.lifecycle);impl.resizing=false;}
    } guard{*impl_};
    if(count>previous){
        std::vector<Impl::Worker> added;
        std::optional<ThreadError> error;
        try {
            added.reserve(count-previous);
            for(std::size_t i=previous;i<count;++i){
                auto control=std::make_shared<Shared::WorkerControl>();
                added.push_back({std::make_unique<Thread>(),control});
                auto result=added.back().thread->start([state,control,home=i%state->queues.size()](ThreadContext&){state->run(home,control);});
                if(!result){error=result.error();break;}
            }
        } catch(...) { error=ThreadError{ThreadErrorCode::start_failed,std::current_exception()}; }
        if(!error){
            std::unique_lock lifecycle(impl_->lifecycle);
            std::lock_guard lock(state->mutex);
            if(state->statistics.state!=RuntimeState::running) error=ThreadError{ThreadErrorCode::invalid_work,{}};
            else {
                for(auto& worker:added) impl_->workers.push_back(std::move(worker));
                for(std::size_t i=previous;i<count;++i) impl_->workers[i].control->enabled.store(true,std::memory_order_release);
                state->admission_paused=false;state->statistics.workers=count;state->touch_stats();
            }
        }
        if(error){
            for(auto& worker:added) if(worker.control) worker.control->retire.store(true,std::memory_order_release);
            state->changed.notify_all();
            for(auto& worker:added) if(worker.thread) (void)worker.thread->join();
            if(previous==0) {
                std::lock_guard lock(state->mutex);
                state->release_slots();
            }
            return std::unexpected(*error);
        }
        state->changed.notify_all();state->notify_stats();return {};
    }
    {
        std::lock_guard lock(state->mutex);
        if(count==0)state->admission_paused=true;
        state->statistics.workers=count;state->touch_stats();
    }
    state->notify_stats();
    if(count==0){
        std::unique_lock lock(state->mutex);
        state->idle.wait(lock,[&]{return state->statistics.pending==0;});
    }
    for(std::size_t i=count;i<previous;++i) impl_->workers[i].control->retire.store(true,std::memory_order_release);
    state->changed.notify_all();
    std::optional<ThreadError> error;
    for(std::size_t i=previous;i>count;--i){
        const auto result=impl_->workers[i-1].thread->join();
        if(!result&&!error) error=result.error();
    }
    if(error) return std::unexpected(*error);
    {
        std::unique_lock lifecycle(impl_->lifecycle);
        impl_->workers.erase(impl_->workers.begin()+static_cast<std::ptrdiff_t>(count),impl_->workers.end());
        std::lock_guard lock(state->mutex);
        if(count==0) state->release_slots();
        state->statistics.workers=count;state->touch_stats();
    }
    state->notify_stats();return {};
}
std::shared_ptr<Runtime::StatsSubscription> Runtime::observe_stats(Task callback) const {
    auto observer=std::make_shared<StatsSubscription>();
    observer->wake=std::move(callback);
    auto& shared=*impl_->shared;
    std::lock_guard lock(shared.observers_mutex);
    auto list=std::make_shared<Shared::Observers>();
    if(const auto previous=std::atomic_load_explicit(&shared.observers,std::memory_order_acquire))
        for(const auto& weak:*previous)
            if(const auto item=weak.lock(); item && item->active.load(std::memory_order_acquire)) list->push_back(item);
    list->push_back(observer);
    std::atomic_store_explicit(&shared.observers,std::shared_ptr<const Shared::Observers>(std::move(list)),std::memory_order_release);
    shared.has_observers.store(true,std::memory_order_release);
    return observer;
}
void Runtime::remove_stats_observer(const std::shared_ptr<StatsSubscription>& observer) const noexcept {
    if(!observer) return;
    observer->active.store(false,std::memory_order_release);
    try {
    auto& shared=*impl_->shared;
    std::lock_guard lock(shared.observers_mutex);
    auto list=std::make_shared<Shared::Observers>();
    if(const auto previous=std::atomic_load_explicit(&shared.observers,std::memory_order_acquire))
        for(const auto& weak:*previous)
            if(const auto item=weak.lock(); item && item!=observer && item->active.load(std::memory_order_acquire)) list->push_back(item);
    if(list->empty()) {
        shared.has_observers.store(false,std::memory_order_release);
        std::atomic_store_explicit(&shared.observers,std::shared_ptr<const Shared::Observers>{},std::memory_order_release);
    }
    else std::atomic_store_explicit(&shared.observers,std::shared_ptr<const Shared::Observers>(std::move(list)),std::memory_order_release);
    } catch (...) {
        // Cancellation remains effective even if pruning the weak index fails.
        // A subsequent registration removes expired entries.
    }
}
ObjectOwner& Runtime::objects() const noexcept { return impl_->objects; }
std::unique_ptr<Session> Runtime::create_session() { return std::make_unique<Session>(*this); }
void Runtime::register_session(const std::shared_ptr<Session::State>& state) {
    std::lock_guard lock(impl_->shared->mutex);
    if (impl_->shared->statistics.state!=RuntimeState::running)
        throw std::logic_error("cannot create a session after Runtime shutdown begins");
    std::lock_guard sessions(impl_->sessions_mutex);
    std::erase_if(impl_->sessions,[](const auto& item){return item.expired();});
    impl_->sessions.push_back(state);
}
bool Runtime::cancel(TaskToken token) {
    return cancel_shared(impl_->shared.get(),token);
}
bool Runtime::pending_shared(void* owner,const TaskToken& token) {
    auto* shared=static_cast<Shared*>(owner);
    std::lock_guard lock(shared->mutex);
    return token.owner_==shared->owner && token.slot_<shared->slots.size() &&
        shared->slots[token.slot_].generation==token.generation_ && shared->slots[token.slot_].status!=Shared::Status::free;
}
std::uint64_t Runtime::stats_revision() const noexcept {
    return impl_->shared->stats_version.load(std::memory_order_acquire);
}
std::pair<RuntimeStats,std::uint64_t> Runtime::versioned_stats() const {
    std::lock_guard lock(impl_->shared->mutex);
    return {impl_->shared->statistics,impl_->shared->stats_version.load(std::memory_order_relaxed)};
}
bool Runtime::wait_shared(void* owner,const TaskToken& token) {
    auto* shared=static_cast<Shared*>(owner);
    std::unique_lock lock(shared->mutex);
    const auto retired=[&] {
        return token.owner_!=shared->owner || token.slot_>=shared->slots.size() ||
            shared->slots[token.slot_].generation!=token.generation_ ||
            shared->slots[token.slot_].status==Shared::Status::free;
    };
    if(retired()) return true;
    if(current_runtime==shared || Retirement::contains_runtime(shared) || host_thread() || HostLoop::is_current_thread()) return false;
    auto found=shared->retirement_waiters.find(token.slot_);
    if (found==shared->retirement_waiters.end()) {
        auto state=std::make_unique<Shared::RetirementWait>();
        found=shared->retirement_waiters.emplace(token.slot_,std::move(state)).first;
    }
    auto& state=*found->second;
    ++state.count;
    struct Waiter final {
        Shared& shared; std::size_t index; Shared::RetirementWait& state;
        ~Waiter() { if (--state.count==0) shared.retirement_waiters.erase(index); }
    } waiter{*shared,token.slot_,state};
    state.changed.wait(lock,retired);
    return true;
}
bool Runtime::cancel_shared(void* owner,const TaskToken& token) {
    Task retired;
    auto* shared=static_cast<Shared*>(owner);
    std::shared_ptr<Lane> lane;
    bool lane_ready=false,drained=false;
    {
        std::lock_guard lock(shared->mutex);
        if (token.owner_ != shared->owner || token.slot_ >= shared->slots.size()) return false;
        auto& slot = shared->slots[token.slot_];
        if (slot.status != Shared::Status::queued || slot.generation != token.generation_) return false;
        const bool was_head=slot.lane && slot.lane->head==token.slot_;
        shared->ready_remove(token.slot_);
        shared->unlink(token.slot_);
        if(was_head && !slot.lane->active && slot.lane->head!=none) {
            shared->ready_add(slot.lane->head); lane_ready=true;
        }
        retired.swap(slot.task);
        lane=slot.lane;
        slot.status=Shared::Status::retiring;
        ++shared->statistics.cancelled;
        ++shared->queues[slot.queue].statistics.cancelled;
        shared->touch_stats();
    }
    {
        Retirement retiring(shared,lane.get());
        retired=nullptr;
    }
    {
        std::lock_guard lock(shared->mutex);
        shared->release(token.slot_);
        drained=shared->statistics.state==RuntimeState::draining && !shared->statistics.pending;
    }
    if (drained) shared->changed.notify_all();
    else if (lane_ready) shared->changed.notify_one();
    shared->notify_stats();
    return true;
}
void Runtime::request_stop(ShutdownMode mode) {
    const auto& shared = impl_->shared;
    bool stats_changed=false;
    {
        std::lock_guard lock(shared->mutex);
        if (shared->statistics.state == RuntimeState::stopped) return;
        const auto previous=shared->statistics.state;
        if (mode == ShutdownMode::cancel_pending) shared->statistics.state = RuntimeState::cancelling;
        else if (shared->statistics.state == RuntimeState::running) shared->statistics.state = RuntimeState::draining;
        stats_changed=previous!=shared->statistics.state;
        if(stats_changed) shared->touch_stats();
    }
    shared->changed.notify_all();
    if(stats_changed) shared->notify_stats();
    if (mode == ShutdownMode::cancel_pending) {
        for (std::size_t i = 0; i < shared->statistics.capacity; ++i) {
            TaskToken token;
            {
                std::lock_guard lock(shared->mutex);
                if (i>=shared->slots.size()) break;
                auto& slot = shared->slots[i];
                if (slot.status != Shared::Status::queued) continue;
                token.owner_=shared->owner; token.slot_=i; token.generation_=slot.generation;
            }
            (void)cancel(token);
        }
    }
}
ThreadResult Runtime::wait_idle() {
    const auto& shared = impl_->shared;
    if (current_runtime == shared.get() || Retirement::contains_runtime(shared.get())) return failure(ThreadErrorCode::self_join);
    std::unique_lock lock(shared->mutex);
    if (host_thread() && shared->statistics.pending) return failure(ThreadErrorCode::would_block);
    shared->idle.wait(lock, [&] { return shared->statistics.pending == 0; });
    return shared->result();
}
ThreadResult Runtime::join() {
    const auto& shared = impl_->shared;
    if (current_runtime == shared.get() || Retirement::contains_runtime(shared.get())) return failure(ThreadErrorCode::self_join);
    if (impl_->objects.is_current()) return failure(ThreadErrorCode::self_join);
    {
        std::lock_guard lock(impl_->sessions_mutex);
        for (const auto& weak:impl_->sessions)
            if (auto state=weak.lock(); state && Session::is_current(state)) return failure(ThreadErrorCode::self_join);
    }
    std::unique_lock lifecycle(impl_->lifecycle);
    if (impl_->joining || impl_->resizing) return failure(ThreadErrorCode::busy);
    if (host_thread() && !impl_->workers.empty()) return failure(ThreadErrorCode::would_block);
    if (impl_->joined) {
        std::lock_guard lock(shared->mutex);
        if (impl_->worker_failure) return std::unexpected(*impl_->worker_failure);
        return shared->result();
    }
    impl_->joining=true;
    const bool join_workers=!impl_->workers_joined;
    lifecycle.unlock();
    request_stop();
    if (join_workers) {
        std::optional<ThreadError> work_error, join_error;
        for (auto& worker:impl_->workers) {
            auto result=worker.thread->join();
            if (!result) {
                if (result.error().code==ThreadErrorCode::work_failed) {
                    if (!work_error) work_error=result.error();
                } else if (!join_error) join_error=result.error();
            }
        }
        lifecycle.lock();
        if (join_error) {
            impl_->joining=false;
            return std::unexpected(*join_error);
        }
        impl_->worker_failure=work_error;
        impl_->workers_joined=true;
        lifecycle.unlock();
    }
    {
        std::unique_lock lock(shared->mutex);
        shared->idle.wait(lock,[&]{return shared->statistics.pending==0;});
    }
    std::size_t session_count=0;
    {
        std::lock_guard lock(impl_->sessions_mutex);
        session_count=impl_->sessions.size();
    }
    std::optional<ThreadError> cleanup_failure;
    while (session_count) {
        std::shared_ptr<Session::State> session;
        {
            std::lock_guard lock(impl_->sessions_mutex);
            session=impl_->sessions[--session_count].lock();
        }
        if (!session) continue;
        auto result=Session::finish(session,true);
        if (!result) {
            if (result.error().code!=ThreadErrorCode::work_failed) {
                lifecycle.lock(); impl_->joining=false;
                return result;
            }
            if (!cleanup_failure) cleanup_failure=result.error();
        }
    }
    auto object_result=impl_->objects.close();
    if (!object_result && object_result.error().code!=ThreadErrorCode::work_failed) {
        lifecycle.lock(); impl_->joining=false;
        return object_result;
    }
    if (!object_result && !cleanup_failure) cleanup_failure=object_result.error();
    lifecycle.lock();
    impl_->joining=false;
    impl_->joined=true;
    if (!impl_->worker_failure && cleanup_failure) impl_->worker_failure=cleanup_failure;
    ThreadResult outcome;
    {
        std::lock_guard lock(shared->mutex);
        shared->statistics.state=RuntimeState::stopped;
        shared->touch_stats();
        outcome=impl_->worker_failure ? ThreadResult(std::unexpected(*impl_->worker_failure)) : shared->result();
    }
    lifecycle.unlock();
    shared->notify_stats();
    return outcome;
}
RuntimeStats Runtime::stats() const { std::lock_guard lock(impl_->shared->mutex); return impl_->shared->statistics; }
std::size_t Runtime::allocated_slots() const { std::lock_guard lock(impl_->shared->mutex); return impl_->shared->slots.size(); }
std::vector<RuntimeQueueStats> Runtime::queue_stats() const {
    std::lock_guard lock(impl_->shared->mutex);
    std::vector<RuntimeQueueStats> result;
    result.reserve(impl_->shared->queues.size());
    for(const auto& queue:impl_->shared->queues) result.push_back(queue.statistics);
    return result;
}
bool Runtime::is_worker_thread() noexcept { return current_runtime!=nullptr; }

struct SerialQueue::Impl {
    std::weak_ptr<Runtime::Shared> shared;
    std::shared_ptr<Runtime::Lane> lane = std::make_shared<Runtime::Lane>();
    explicit Impl(Runtime& runtime) : shared(runtime.impl_->shared) {}
};
SerialQueue::SerialQueue(Runtime& runtime) : impl_(std::make_unique<Impl>(runtime)) {}
SerialQueue::~SerialQueue() {
    const auto result=close();
    if (!result) std::terminate();
}
Submission SerialQueue::post(Task task, Priority priority) const {
    return Runtime::submit(impl_->shared.lock(), impl_->lane, std::move(task), priority);
}
bool SerialQueue::is_current() const noexcept { return current_lane == impl_->lane.get(); }
void SerialQueue::request_close() const {
    const auto shared=impl_->shared.lock();
    if (!shared) return;
    {
        std::lock_guard lock(shared->mutex);
        impl_->lane->closed=true;
    }
    for(std::size_t i=0;i<shared->statistics.capacity;++i) {
        TaskToken token;
        {
            std::lock_guard lock(shared->mutex);
            if(i>=shared->slots.size()) break;
            const auto& slot=shared->slots[i];
            if(slot.status!=Runtime::Shared::Status::queued || slot.lane!=impl_->lane) continue;
            token.owner_=shared->owner; token.slot_=i; token.generation_=slot.generation;
        }
        (void)Runtime::cancel_shared(shared.get(),token);
    }
}
ThreadResult SerialQueue::close() const {
    if(is_current() || Retirement::contains(impl_->lane.get())) return failure(ThreadErrorCode::self_join);
    request_close();
    const auto shared=impl_->shared.lock();
    if(!shared) return {};
    std::unique_lock lock(shared->mutex);
    if(impl_->lane->pending && (host_thread() || current_runtime==shared.get()))
        return failure(ThreadErrorCode::would_block);
    impl_->lane->idle.wait(lock,[&]{return impl_->lane->pending==0;});
    return {};
}

} // namespace ii::runtime
