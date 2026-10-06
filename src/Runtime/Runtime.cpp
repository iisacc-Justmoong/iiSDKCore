#include "Runtime.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
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
};

struct Runtime::Shared {
    enum class Status { free, queued, executing, retiring };
    struct Slot {
        Task task;
        std::shared_ptr<Lane> lane;
        std::size_t previous = none, next = none;
        std::uint64_t sequence = 0, generation = 0;
        Priority priority = Priority::normal;
        Status status = Status::free;
    };
    mutable std::mutex mutex;
    std::condition_variable changed, idle;
    std::vector<Slot> slots;
    std::vector<std::size_t> free;
    const std::uint64_t owner = TaskToken::next_owner();
    std::uint64_t sequence = 0;
    std::size_t cursor = 0;
    RuntimeStats statistics;
    std::exception_ptr first_failure;

    explicit Shared(std::size_t capacity) : slots(capacity) {
        if (!capacity) throw std::invalid_argument("Runtime capacity must be positive");
        free.reserve(capacity);
        for (std::size_t i = capacity; i; --i) free.push_back(i - 1);
        statistics.capacity = capacity;
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
        if (slot.lane) --slot.lane->pending;
        slot.lane.reset();
        slot.status = Status::free;
        slot.previous=slot.next=none; slot.sequence=0; slot.priority=Priority::normal;
        free.push_back(index);
        --statistics.pending;
        idle.notify_all();
    }
    std::size_t select() {
        for (std::size_t attempt = 0; attempt < wheel.size(); ++attempt) {
            const auto priority = wheel[cursor];
            cursor = (cursor + 1) % wheel.size();
            std::size_t chosen = none;
            for (std::size_t i = 0; i < slots.size(); ++i) {
                const auto& slot = slots[i];
                if (slot.status != Status::queued || slot.priority != priority) continue;
                if (slot.lane && (slot.lane->active || slot.lane->head != i)) continue;
                if (chosen == none || slot.sequence < slots[chosen].sequence) chosen = i;
            }
            if (chosen != none) return chosen;
        }
        return none;
    }
    ThreadResult result() const {
        if (first_failure) return failure(ThreadErrorCode::work_failed, first_failure);
        return {};
    }
    void run() {
        current_runtime = this;
        for (;;) {
            std::size_t index = none;
            Task task;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] {
                    if (statistics.state == RuntimeState::cancelling || statistics.state == RuntimeState::stopped)
                        return true;
                    index = select();
                    return index != none || (statistics.state == RuntimeState::draining && !statistics.pending);
                });
                if (index == none) break;
                auto& slot = slots[index];
                unlink(index);
                if (slot.lane) slot.lane->active = true;
                current_lane = slot.lane.get();
                slot.status = Status::executing;
                ++statistics.active;
                task.swap(slot.task);
            }
            std::exception_ptr error;
            try { task(); } catch (...) { error = std::current_exception(); }
            // Captures may reenter submission; never destroy them under the lock.
            task = nullptr;
            current_lane = nullptr;
            {
                std::lock_guard lock(mutex);
                auto& slot = slots[index];
                if (slot.lane) slot.lane->active = false;
                if (error) {
                    ++statistics.failed;
                    if (!first_failure) first_failure = error;
                }
                ++statistics.executed;
                --statistics.active;
                release(index);
            }
            changed.notify_all();
        }
        current_runtime = nullptr;
    }
};

struct Runtime::Impl {
    std::shared_ptr<Shared> shared;
    std::vector<std::unique_ptr<Thread>> workers;
    std::mutex lifecycle;
    bool joining = false, joined = false;
    explicit Impl(RuntimeOptions options) : shared(std::make_shared<Shared>(options.capacity)) {
        if (!Thread::supported()) throw std::runtime_error("Runtime requires native or WASM pthread workers");
        const auto count = options.workers ? options.workers :
            std::clamp<std::size_t>(std::thread::hardware_concurrency(), 2, 4);
        shared->statistics.workers = count;
        workers.reserve(count);
        for (std::size_t i = 0; i < count; ++i) workers.push_back(std::make_unique<Thread>());
        try {
            for (auto& worker : workers) {
                auto started = worker->start([state = shared](ThreadContext&) { state->run(); });
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
            for (auto& owned : workers) (void)owned->join();
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
    std::lock_guard lock(shared->mutex);
    auto reject = [&](SubmitError error) { ++shared->statistics.rejected; return Submission{{}, error}; };
    if (!task || (priority != Priority::high && priority != Priority::normal && priority != Priority::background))
        return reject(SubmitError::invalid);
    if (shared->statistics.state != RuntimeState::running) return reject(SubmitError::stopped);
    if (lane && lane->closed) return reject(SubmitError::stopped);
    if (shared->free.empty()) return reject(SubmitError::full);
    const auto index = shared->free.back();
    shared->free.pop_back();
    auto& slot = shared->slots[index];
    slot.task.swap(task);
    slot.lane = lane;
    slot.priority = priority;
    slot.sequence = ++shared->sequence;
    ++slot.generation;
    slot.status = Shared::Status::queued;
    if (lane) {
        ++lane->pending;
        slot.previous = lane->tail;
        if (lane->tail != none) shared->slots[lane->tail].next = index;
        else lane->head = index;
        lane->tail = index;
    }
    ++shared->statistics.submitted;
    ++shared->statistics.pending;
    shared->statistics.high_watermark = std::max(shared->statistics.high_watermark, shared->statistics.pending);
    TaskToken token;
    token.owner_ = shared->owner;
    token.slot_ = index;
    token.generation_ = slot.generation;
    token.executor_=shared;
    token.cancel_=&Runtime::cancel_shared;
    token.pending_=&Runtime::pending_shared;
    shared->changed.notify_one();
    return {token, SubmitError::none};
}
Submission Runtime::post(Task task, Priority priority) { return submit(impl_->shared, {}, std::move(task), priority); }
bool Runtime::cancel(TaskToken token) {
    return cancel_shared(impl_->shared.get(),token);
}
bool Runtime::pending_shared(void* owner,const TaskToken& token) {
    auto* shared=static_cast<Shared*>(owner);
    std::lock_guard lock(shared->mutex);
    return token.owner_==shared->owner && token.slot_<shared->slots.size() &&
        shared->slots[token.slot_].generation==token.generation_ && shared->slots[token.slot_].status!=Shared::Status::free;
}
bool Runtime::cancel_shared(void* owner,const TaskToken& token) {
    Task retired;
    auto* shared=static_cast<Shared*>(owner);
    std::shared_ptr<Lane> lane;
    {
        std::lock_guard lock(shared->mutex);
        if (token.owner_ != shared->owner || token.slot_ >= shared->slots.size()) return false;
        auto& slot = shared->slots[token.slot_];
        if (slot.status != Shared::Status::queued || slot.generation != token.generation_) return false;
        shared->unlink(token.slot_);
        retired.swap(slot.task);
        lane=slot.lane;
        slot.status=Shared::Status::retiring;
        ++shared->statistics.cancelled;
    }
    {
        Retirement retiring(shared,lane.get());
        retired=nullptr;
    }
    {
        std::lock_guard lock(shared->mutex);
        shared->release(token.slot_);
    }
    shared->changed.notify_all();
    return true;
}
void Runtime::request_stop(ShutdownMode mode) {
    const auto& shared = impl_->shared;
    {
        std::lock_guard lock(shared->mutex);
        if (shared->statistics.state == RuntimeState::stopped) return;
        if (mode == ShutdownMode::cancel_pending) shared->statistics.state = RuntimeState::cancelling;
        else if (shared->statistics.state == RuntimeState::running) shared->statistics.state = RuntimeState::draining;
    }
    shared->changed.notify_all();
    if (mode == ShutdownMode::cancel_pending) {
        for (std::size_t i = 0; i < shared->slots.size(); ++i) {
            TaskToken token;
            {
                std::lock_guard lock(shared->mutex);
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
    if (host_thread()) return failure(ThreadErrorCode::would_block);
    std::unique_lock lock(shared->mutex);
    shared->idle.wait(lock, [&] { return shared->statistics.pending == 0; });
    return shared->result();
}
ThreadResult Runtime::join() {
    const auto& shared = impl_->shared;
    if (current_runtime == shared.get() || Retirement::contains_runtime(shared.get())) return failure(ThreadErrorCode::self_join);
    if (host_thread()) return failure(ThreadErrorCode::would_block);
    std::unique_lock lifecycle(impl_->lifecycle);
    if (impl_->joining) return failure(ThreadErrorCode::busy);
    if (!impl_->joined) {
        impl_->joining = true;
        lifecycle.unlock();
        request_stop();
        std::optional<ThreadError> error;
        for (auto& worker : impl_->workers) {
            auto result = worker->join();
            if (!result && !error) error = result.error();
        }
        lifecycle.lock();
        impl_->joining = false;
        if (error) return std::unexpected(*error);
        impl_->joined = true;
        std::unique_lock lock(shared->mutex);
        shared->idle.wait(lock, [&] { return shared->statistics.pending == 0; });
        shared->statistics.state = RuntimeState::stopped;
    }
    std::lock_guard lock(shared->mutex);
    return shared->result();
}
RuntimeStats Runtime::stats() const { std::lock_guard lock(impl_->shared->mutex); return impl_->shared->statistics; }
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
    for(std::size_t i=0;i<shared->slots.size();++i) {
        TaskToken token;
        {
            std::lock_guard lock(shared->mutex);
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
    shared->idle.wait(lock,[&]{return impl_->lane->pending==0;});
    return {};
}

} // namespace ii::runtime
