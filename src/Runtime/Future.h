#pragma once
#include "Runtime.h"
#include "HostLoop.h"
#include "../Core/Result/Result.h"
#include <array>
#include <concepts>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
#endif

namespace ii::runtime {
enum class FutureStatus { invalid, pending, running, succeeded, failed, cancelled, rejected, abandoned };
enum class FutureWait { ready, timed_out, would_block, invalid };
struct FutureProgress final {
    std::size_t minimum = 0, maximum = 100, value = 0;
    std::string text{};
};
struct FutureSnapshot final {
    FutureStatus status = FutureStatus::invalid;
    SubmitError admission = SubmitError::none;
    bool cancellation_requested = false, suspension_requested = false;
    std::size_t result_count = 0, suspended_producers = 0;
    std::uint64_t revision = 0;
    FutureProgress progress{};
    std::optional<core::Error> error{};
};
struct PromiseOptions final { std::size_t max_results = 1024; };

namespace future_detail {
inline bool terminal(FutureStatus status) {
    return status != FutureStatus::pending && status != FutureStatus::running;
}
inline bool blocking_forbidden() {
    if (Runtime::is_worker_thread() || HostLoop::is_current_thread()) return true;
#if defined(__EMSCRIPTEN__)
    if (emscripten_is_main_browser_thread()) return true;
#endif
    return false;
}
struct Notification final { std::function<void()> notify; };
struct Control final {
    mutable std::mutex mutex;
    std::condition_variable changed;
    FutureSnapshot snapshot{.status = FutureStatus::pending};
    std::stop_source stop;
    std::stop_token owner;
    std::vector<TaskToken> tokens;
    std::array<std::weak_ptr<Notification>, 16> observers;

    bool interrupted() const {
        return snapshot.cancellation_requested || owner.stop_requested();
    }
    void notify() {
        changed.notify_all();
        std::array<std::shared_ptr<Notification>, 16> ready;
        {
            std::lock_guard lock(mutex);
            for (std::size_t i = 0; i < observers.size(); ++i) ready[i] = observers[i].lock();
        }
        // Observers schedule onto their own executors; no callback runs under
        // the future lock, and observer admission cannot fail the computation.
        for (const auto& item : ready) if (item) {
            try { item->notify(); } catch (...) {}
        }
    }
    bool finish(FutureStatus status, std::optional<core::Error> error = {}) {
        {
            std::lock_guard lock(mutex);
            if (terminal(snapshot.status)) return false;
            if ((status == FutureStatus::succeeded || status == FutureStatus::abandoned) && interrupted()) {
                status = FutureStatus::cancelled;
                error = core::Error{core::ErrorCode::cancelled, "Concurrent producer cancelled", {}};
            }
            snapshot.status = status;
            snapshot.error = std::move(error);
            snapshot.suspension_requested = false;
            ++snapshot.revision;
        }
        notify();
        return true;
    }
};
template<class T> using Stored = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
template<class T> struct State final {
    Control control;
    std::size_t max_results;
    std::vector<std::shared_ptr<const Stored<T>>> values;
    explicit State(std::size_t limit, std::stop_token owner) : max_results(limit) {
        control.owner = owner;
    }
};
inline core::Error wait_error(FutureWait result) {
    return {result == FutureWait::would_block ? core::ErrorCode::wrong_executor : core::ErrorCode::invalid_argument,
            "Future wait requires a valid future and an external blocking thread", {}};
}
}
template<class T> class Promise;
template<class T> class FutureWatcher;
namespace concurrent_detail { template<class T, class Finish> struct Batch; }

// Copyable observation and control endpoint. Results are immutable shared
// values so move-only results can be observed without moving from other readers.
template<class T> class Future final {
public:
    Future() = default;
    [[nodiscard]] bool valid() const noexcept { return bool(state_); }
    [[nodiscard]] FutureSnapshot snapshot() const {
        if (!state_) return {};
        std::lock_guard lock(state_->control.mutex);
        auto result = state_->control.snapshot;
        result.cancellation_requested = state_->control.interrupted();
        return result;
    }
    [[nodiscard]] bool ready() const {
        if (!state_) return false;
        std::lock_guard lock(state_->control.mutex);
        return future_detail::terminal(state_->control.snapshot.status);
    }
    [[nodiscard]] std::shared_ptr<const T> result_at(std::size_t index) const requires (!std::is_void_v<T>) {
        if (!state_) return {};
        std::lock_guard lock(state_->control.mutex);
        return index < state_->values.size() ? state_->values[index] : nullptr;
    }
    [[nodiscard]] std::vector<std::shared_ptr<const T>> results() const requires (!std::is_void_v<T>) {
        std::vector<std::shared_ptr<const T>> result;
        if (!state_) return result;
        std::lock_guard lock(state_->control.mutex);
        result.reserve(state_->control.snapshot.result_count);
        for (const auto& value : state_->values) if (value) result.push_back(value);
        return result;
    }
    bool cancel() const {
        if (!state_) return false;
        auto& control = state_->control;
        std::vector<TaskToken> tokens;
        {
            std::lock_guard lock(control.mutex);
            if (future_detail::terminal(control.snapshot.status) || control.interrupted()) return false;
            tokens = control.tokens;
            control.snapshot.cancellation_requested = true;
            ++control.snapshot.revision;
        }
        control.stop.request_stop();
        control.notify();
        for (const auto& token : tokens) (void)token.cancel();
        return true;
    }
    bool suspend() const { return set_suspended(true); }
    bool resume() const { return set_suspended(false); }
    [[nodiscard]] FutureWait wait_for(Clock::duration duration) const {
        if (!state_) return FutureWait::invalid;
        auto& control = state_->control;
        std::unique_lock lock(control.mutex);
        if (future_detail::terminal(control.snapshot.status)) return FutureWait::ready;
        if (future_detail::blocking_forbidden()) return FutureWait::would_block;
        return control.changed.wait_for(lock, duration, [&] { return future_detail::terminal(control.snapshot.status); })
            ? FutureWait::ready : FutureWait::timed_out;
    }
    [[nodiscard]] FutureWait wait() const {
        if (!state_) return FutureWait::invalid;
        auto& control = state_->control;
        std::unique_lock lock(control.mutex);
        if (future_detail::terminal(control.snapshot.status)) return FutureWait::ready;
        if (future_detail::blocking_forbidden()) return FutureWait::would_block;
        control.changed.wait(lock, [&] { return future_detail::terminal(control.snapshot.status); });
        return FutureWait::ready;
    }
    [[nodiscard]] core::Result<T> get() const requires (!std::is_void_v<T> && std::copy_constructible<T>) {
        if (auto error = completion_error()) return std::unexpected(std::move(*error));
        std::shared_ptr<const T> value;
        {
            std::lock_guard lock(state_->control.mutex);
            for (const auto& candidate : state_->values) if (candidate) { value = candidate; break; }
        }
        if (!value) return core::fail(core::ErrorCode::not_found, "Future has no published result");
        return *value;
    }
    [[nodiscard]] core::Result<void> get() const requires std::is_void_v<T> {
        if (auto error = completion_error()) return std::unexpected(std::move(*error));
        return {};
    }
    template<class U = T> requires std::same_as<U, T> && std::copy_constructible<U>
    [[nodiscard]] core::Result<std::vector<U>> get_all() const {
        if (auto error = completion_error()) return std::unexpected(std::move(*error));
        std::vector<U> result;
        for (const auto& value : results()) result.push_back(*value);
        return result;
    }
private:
    friend class Promise<T>;
    friend class FutureWatcher<T>;
    explicit Future(std::shared_ptr<future_detail::State<T>> state) : state_(std::move(state)) {}
    bool set_suspended(bool suspend) const {
        if (!state_) return false;
        auto& control = state_->control;
        {
            std::lock_guard lock(control.mutex);
            if (future_detail::terminal(control.snapshot.status) || control.interrupted() ||
                control.snapshot.suspension_requested == suspend) return false;
            control.snapshot.suspension_requested = suspend;
            ++control.snapshot.revision;
        }
        control.notify();
        return true;
    }
    std::optional<core::Error> completion_error() const {
        const auto waited = wait();
        if (waited != FutureWait::ready) return future_detail::wait_error(waited);
        const auto current = snapshot();
        if (current.status == FutureStatus::succeeded) return {};
        if (current.error) return current.error;
        return core::Error{core::ErrorCode::failed, "Future did not succeed", {}};
    }
    bool observe(const std::shared_ptr<future_detail::Notification>& observer) const {
        if (!state_) return false;
        std::lock_guard lock(state_->control.mutex);
        for (auto& slot : state_->control.observers) if (slot.expired()) { slot = observer; return true; }
        return false;
    }
    std::shared_ptr<future_detail::State<T>> state_;
};

// One movable producer, safely callable by several externally synchronized
// owners of its lifetime. Removing an unfinished producer always settles it.
template<class T> class Promise final {
public:
    explicit Promise(PromiseOptions options = {}, std::stop_token owner = {})
        : state_(std::make_shared<future_detail::State<T>>(options.max_results, owner)) {}
    ~Promise() { abandon(); }
    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;
    Promise(Promise&&) noexcept = default;
    Promise& operator=(Promise&& other) noexcept {
        if (this != &other) { abandon(); state_ = std::move(other.state_); }
        return *this;
    }
    [[nodiscard]] Future<T> future() const { return Future<T>(state_); }
    bool start() {
        auto& control = state_->control;
        {
            std::lock_guard lock(control.mutex);
            if (control.snapshot.status != FutureStatus::pending) return false;
            control.snapshot.status = FutureStatus::running;
            ++control.snapshot.revision;
        }
        control.notify(); return true;
    }
    template<class U> requires (!std::is_void_v<T> && std::constructible_from<T, U&&>)
    [[nodiscard]] core::Result<void> add_result(U&& value, std::optional<std::size_t> index = {}) {
        auto& control = state_->control;
        auto validate = [&]() -> core::Result<void> {
            if (future_detail::terminal(control.snapshot.status))
                return core::fail(core::ErrorCode::closed, "Future result is already final");
            if (control.interrupted()) return core::fail(core::ErrorCode::cancelled, "Result producer cancelled");
            const auto at = index.value_or(state_->values.size());
            if (at >= state_->max_results) return core::fail(core::ErrorCode::full, "Future result budget exceeded");
            if (at < state_->values.size() && state_->values[at])
                return core::fail(core::ErrorCode::already_exists, "Future result index already published");
            return {};
        };
        {
            std::lock_guard lock(control.mutex);
            auto valid = validate(); if (!valid) return valid;
        }
        // User construction runs outside the state lock. Concurrent appenders
        // choose their append index only when publication is committed.
        auto stored = std::make_shared<const T>(std::forward<U>(value));
        {
            std::lock_guard lock(control.mutex);
            auto valid = validate(); if (!valid) return valid;
            const auto at = index.value_or(state_->values.size());
            if (state_->values.size() <= at) state_->values.resize(at + 1);
            state_->values[at] = std::move(stored);
            ++control.snapshot.result_count; ++control.snapshot.revision;
        }
        control.notify(); return {};
    }
    [[nodiscard]] bool is_cancelled() const {
        std::lock_guard lock(state_->control.mutex); return state_->control.interrupted();
    }
    [[nodiscard]] std::stop_token stop_token() const noexcept { return state_->control.stop.get_token(); }
    [[nodiscard]] std::stop_token owner_stop_token() const noexcept { return state_->control.owner; }
    [[nodiscard]] core::Result<void> checkpoint() {
        auto& control = state_->control;
        std::stop_callback wake_owner(control.owner, [&] { control.changed.notify_all(); });
        std::unique_lock lock(control.mutex);
        if (control.snapshot.suspension_requested && !control.interrupted() &&
            !future_detail::terminal(control.snapshot.status)) {
            if (HostLoop::is_current_thread())
                return core::fail(core::ErrorCode::wrong_executor, "Cannot suspend a host dispatch thread");
#if defined(__EMSCRIPTEN__)
            if (emscripten_is_main_browser_thread())
                return core::fail(core::ErrorCode::wrong_executor, "Cannot suspend the browser thread");
#endif
            ++control.snapshot.suspended_producers; ++control.snapshot.revision;
            lock.unlock(); control.notify(); lock.lock();
            control.changed.wait(lock, [&] {
                return !control.snapshot.suspension_requested || control.interrupted() ||
                    future_detail::terminal(control.snapshot.status);
            });
            --control.snapshot.suspended_producers; ++control.snapshot.revision;
            lock.unlock(); control.notify(); lock.lock();
        }
        if (control.interrupted()) return core::fail(core::ErrorCode::cancelled, "Concurrent producer cancelled");
        if (future_detail::terminal(control.snapshot.status))
            return core::fail(core::ErrorCode::closed, "Concurrent producer already finished");
        return {};
    }
    [[nodiscard]] core::Result<void> set_progress_range(std::size_t minimum, std::size_t maximum) {
        if (minimum > maximum) return core::fail(core::ErrorCode::invalid_argument, "Invalid future progress range");
        auto& control = state_->control;
        {
            std::lock_guard lock(control.mutex);
            if (future_detail::terminal(control.snapshot.status))
                return core::fail(core::ErrorCode::closed, "Future progress is final");
            control.snapshot.progress = {minimum, maximum, minimum, {}};
            ++control.snapshot.revision;
        }
        control.notify(); return {};
    }
    [[nodiscard]] core::Result<void> set_progress(std::size_t value, std::string text = {}) {
        auto& control = state_->control;
        {
            std::lock_guard lock(control.mutex);
            if (future_detail::terminal(control.snapshot.status))
                return core::fail(core::ErrorCode::closed, "Future progress is final");
            auto& progress = control.snapshot.progress;
            if (value < progress.minimum || value > progress.maximum)
                return core::fail(core::ErrorCode::out_of_range, "Future progress is outside its range");
            progress.value = value; progress.text = std::move(text); ++control.snapshot.revision;
        }
        control.notify(); return {};
    }
    bool finish() { return state_->control.finish(FutureStatus::succeeded); }
    bool fail(core::Error error) { return state_->control.finish(FutureStatus::failed, std::move(error)); }
    void advance_progress() {
        auto& control = state_->control;
        {
            std::lock_guard lock(control.mutex);
            if (future_detail::terminal(control.snapshot.status)) return;
            if (control.snapshot.progress.value < control.snapshot.progress.maximum)
                ++control.snapshot.progress.value;
            ++control.snapshot.revision;
        }
        control.notify();
    }
private:
    friend class Concurrent;
    template<class U, class Finish> friend struct concurrent_detail::Batch;
    void abandon() {
        if (state_) (void)state_->control.finish(FutureStatus::abandoned,
            core::Error{core::ErrorCode::closed, "Future producer removed before completion", {}});
    }
    void admitted(Submission submitted) {
        auto& control = state_->control;
        bool cancel = false;
        {
            std::lock_guard lock(control.mutex);
            if (!submitted) control.snapshot.admission = submitted.error;
            else { control.tokens.push_back(submitted.token); cancel = control.interrupted(); }
        }
        if (cancel) (void)submitted.token.cancel();
    }
    bool reject(SubmitError error) {
        admitted({{}, error});
        return state_->control.finish(FutureStatus::rejected, core::Error{
            error == SubmitError::full ? core::ErrorCode::full : core::ErrorCode::closed,
            "Concurrent admission rejected", {}});
    }
    std::shared_ptr<future_detail::State<T>> state_;
};
} // namespace ii::runtime
