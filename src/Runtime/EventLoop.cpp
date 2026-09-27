#include "EventLoop.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace ii::runtime {
namespace {
std::atomic<std::uint64_t> next_task_owner{1};
constexpr std::array wheel{Priority::high, Priority::high, Priority::high,
    Priority::high, Priority::normal, Priority::normal, Priority::background};

Clock::time_point add_saturated(Clock::time_point now, Clock::duration delay) {
    if (delay <= Clock::duration::zero()) return now;
    if (delay > Clock::time_point::max() - now) return Clock::time_point::max();
    return now + delay;
}
void validate_budget(PumpBudget budget) {
    if (budget.max_time < Clock::duration::zero())
        throw std::invalid_argument("Pump budget duration must not be negative");
}
}

std::uint64_t TaskToken::next_owner() { return next_task_owner.fetch_add(1); }

struct EventLoop::Impl {
    enum class Status { free, queued, executing };
    struct Slot {
        Task task;
        Clock::time_point deadline{};
        Clock::duration interval{};
        std::uint64_t generation = 0, sequence = 0;
        Priority priority = Priority::normal;
        Status status = Status::free;
        bool cancelled = false;
    };
    struct DispatchGuard {
        Impl& impl;
        explicit DispatchGuard(Impl& value) : impl(value) {
            if (impl.dispatching.test_and_set())
                throw std::logic_error("EventLoop dispatch is already active (recursive or concurrent pump)");
        }
        ~DispatchGuard() { impl.dispatching.clear(); }
    };
    explicit Impl(std::size_t capacity) : slots(capacity), owner(TaskToken::next_owner()) {
        if (capacity == 0) throw std::invalid_argument("EventLoop capacity must be positive");
        free.reserve(capacity);
        for (std::size_t i = capacity; i > 0; --i) free.push_back(i - 1);
        statistics.capacity = capacity;
    }
    void release(std::size_t index) {
        slots[index].status = Status::free;
        free.push_back(index);
        --statistics.pending;
    }
    std::optional<Clock::time_point> deadline() const {
        std::optional<Clock::time_point> next;
        if (state != LoopState::ready) return next;
        for (const auto& slot : slots)
            if (slot.status == Status::queued && (!next || slot.deadline < *next))
                next = slot.deadline;
        return next;
    }
    std::optional<std::size_t> select(Clock::time_point now) {
        for (std::size_t attempt = 0; attempt < wheel.size(); ++attempt) {
            const auto priority = wheel[cursor];
            cursor = (cursor + 1) % wheel.size();
            std::optional<std::size_t> chosen;
            for (std::size_t i = 0; i < slots.size(); ++i) {
                const auto& slot = slots[i];
                if (slot.status == Status::queued && slot.deadline <= now &&
                    slot.priority == priority && (!chosen || slot.sequence < slots[*chosen].sequence))
                    chosen = i;
            }
            if (chosen) return chosen;
        }
        return std::nullopt;
    }
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::atomic_flag dispatching = ATOMIC_FLAG_INIT;
    std::vector<Slot> slots;
    std::vector<std::size_t> free;
    const std::uint64_t owner;
    std::uint64_t sequence = 0;
    std::size_t cursor = 0;
    LoopState state = LoopState::ready;
    LoopStats statistics;
};

EventLoop::EventLoop(std::size_t capacity) : impl_(std::make_unique<Impl>(capacity)) {}
EventLoop::~EventLoop() = default;

Submission EventLoop::submit(Clock::time_point deadline, Clock::duration interval,
                             Task task, Priority priority) {
    std::lock_guard lock(impl_->mutex);
    auto reject = [&](SubmitError error) {
        ++impl_->statistics.rejected;
        return Submission{{}, error};
    };
    if (!task || (priority != Priority::high && priority != Priority::normal &&
                  priority != Priority::background) || interval < Clock::duration::zero())
        return reject(SubmitError::invalid);
    if (impl_->state == LoopState::stopped) return reject(SubmitError::stopped);
    if (impl_->free.empty()) return reject(SubmitError::full);
    const auto index = impl_->free.back();
    impl_->free.pop_back();
    auto& slot = impl_->slots[index];
    slot.task.swap(task);
    slot.deadline = deadline;
    slot.interval = interval;
    slot.priority = priority;
    slot.sequence = ++impl_->sequence;
    ++slot.generation;
    slot.cancelled = false;
    slot.status = Impl::Status::queued;
    ++impl_->statistics.submitted;
    ++impl_->statistics.pending;
    impl_->statistics.high_watermark = std::max(impl_->statistics.high_watermark,
                                                impl_->statistics.pending);
    TaskToken token;
    token.owner_ = impl_->owner;
    token.generation_ = slot.generation;
    token.slot_ = index;
    impl_->changed.notify_one();
    return {token, SubmitError::none};
}

Submission EventLoop::post(Task task, Priority priority) {
    return submit(Clock::now(), {}, std::move(task), priority);
}
Submission EventLoop::at(Clock::time_point deadline, Task task, Priority priority) {
    // Clamp past deadlines to avoid overflow in timer arithmetic.
    return submit(std::max(deadline, Clock::now()), {}, std::move(task), priority);
}
Submission EventLoop::after(Clock::duration delay, Task task, Priority priority) {
    return submit(add_saturated(Clock::now(), delay), {}, std::move(task), priority);
}
Submission EventLoop::every(Clock::duration interval, Task task, Priority priority) {
    if (interval <= Clock::duration::zero()) {
        std::lock_guard lock(impl_->mutex);
        ++impl_->statistics.rejected;
        return {{}, SubmitError::invalid};
    }
    return submit(add_saturated(Clock::now(), interval), interval, std::move(task), priority);
}

bool EventLoop::cancel(TaskToken token) {
    Task retired;
    {
        std::lock_guard lock(impl_->mutex);
        if (token.owner_ != impl_->owner || token.slot_ >= impl_->slots.size()) return false;
        auto& slot = impl_->slots[token.slot_];
        if (slot.generation != token.generation_ || slot.status == Impl::Status::free ||
            slot.cancelled) return false;
        slot.cancelled = true;
        ++impl_->statistics.cancelled;
        if (slot.status == Impl::Status::queued) {
            retired.swap(slot.task);
            impl_->release(token.slot_);
        }
        impl_->changed.notify_one();
    }
    // A callback capture's destructor may post work; never destroy it under the lock.
    return true;
}

std::size_t EventLoop::dispatch(PumpBudget budget) {
    const auto end = add_saturated(Clock::now(), budget.max_time);
    std::size_t executed = 0;
    while (executed < budget.max_callbacks && Clock::now() < end) {
        Task task;
        std::size_t index;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->state != LoopState::ready) break;
            const auto chosen = impl_->select(Clock::now());
            if (!chosen) break;
            index = *chosen;
            auto& slot = impl_->slots[index];
            slot.status = Impl::Status::executing;
            // std::function's moved-from state is unspecified: swap with an
            // empty function so reused slots never retain callback captures.
            task.swap(slot.task);
        }
        std::exception_ptr failure;
        try { task(); } catch (...) { failure = std::current_exception(); }
        {
            std::lock_guard lock(impl_->mutex);
            auto& slot = impl_->slots[index];
            ++impl_->statistics.executed;
            if (!failure && !slot.cancelled && slot.interval > Clock::duration::zero() &&
                impl_->state != LoopState::stopped) {
                const auto elapsed = Clock::now() - slot.deadline;
                const auto missed = elapsed / slot.interval;
                impl_->statistics.missed_ticks += static_cast<std::uint64_t>(missed);
                // Preserve phase, but never run an unbounded catch-up burst.
                slot.deadline = add_saturated(slot.deadline + elapsed,
                                              slot.interval - elapsed % slot.interval);
                slot.task.swap(task);
                slot.sequence = ++impl_->sequence;
                slot.status = Impl::Status::queued;
            } else {
                impl_->release(index);
            }
        }
        ++executed;
        if (failure) std::rethrow_exception(failure);
    }
    return executed;
}

std::size_t EventLoop::poll(PumpBudget budget) {
    validate_budget(budget);
    Impl::DispatchGuard guard(*impl_);
    return dispatch(budget);
}

void EventLoop::run(PumpBudget budget) {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    (void)budget;
    throw std::logic_error("WASM requires host-driven poll() or BrowserLoop; run() would block the host");
#else
#if defined(__EMSCRIPTEN__)
    if (emscripten_is_main_browser_thread())
        throw std::logic_error("EventLoop::run cannot block the browser main thread");
#endif
    validate_budget(budget);
    if (budget.max_callbacks == 0 || budget.max_time == Clock::duration::zero())
        throw std::invalid_argument("Blocking run requires a positive pump budget");
    Impl::DispatchGuard guard(*impl_);
    for (;;) {
        dispatch(budget);
        std::unique_lock lock(impl_->mutex);
        if (impl_->state == LoopState::stopped) return;
        const auto next = impl_->deadline();
        if (next && *next <= Clock::now()) continue;
        if (next) impl_->changed.wait_until(lock, *next);
        else impl_->changed.wait(lock);
        // Recompute after every wake: posts, cancellations and lifecycle changes
        // share this mutex, preventing the check/sleep lost-wakeup race.
    }
#endif
}

void EventLoop::suspend() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state == LoopState::ready) impl_->state = LoopState::suspended;
    impl_->changed.notify_one();
}
void EventLoop::resume() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state == LoopState::suspended) impl_->state = LoopState::ready;
    impl_->changed.notify_one();
}
void EventLoop::request_stop() {
    std::lock_guard lock(impl_->mutex);
    impl_->state = LoopState::stopped;
    impl_->changed.notify_one();
}
LoopState EventLoop::state() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->state;
}
LoopStats EventLoop::stats() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->statistics;
}
std::optional<Clock::time_point> EventLoop::next_deadline() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->deadline();
}

Platform current_platform() noexcept {
#if defined(__EMSCRIPTEN__)
    return Platform::wasm;
#elif defined(__ANDROID__)
    return Platform::android;
#elif defined(_WIN32)
    return Platform::windows;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return Platform::ios;
#elif defined(__APPLE__)
    return Platform::macos;
#elif defined(__linux__)
    return Platform::linux_os;
#else
    return Platform::unknown;
#endif
}
std::string_view platform_name() noexcept {
    switch (current_platform()) {
    case Platform::macos: return "macOS";
    case Platform::windows: return "Windows";
    case Platform::linux_os: return "Linux";
    case Platform::ios: return "iOS";
    case Platform::android: return "Android";
    case Platform::wasm: return "WASM";
    case Platform::unknown: return "unknown";
    }
    return "unknown";
}

} // namespace ii::runtime
