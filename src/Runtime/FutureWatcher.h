#pragma once
#include "Future.h"
#include "TaskScope.h"
#include <atomic>
#include <stdexcept>

namespace ii::runtime {
// A bounded, coalescing subscription. The executor outlives the watcher; close
// is the callback/capture removal barrier. No polling thread is created.
template<class T> class FutureWatcher final {
    struct Shared;
    struct Delivery final {
        std::weak_ptr<Shared> receiver;
        std::uint64_t generation;
        std::atomic<bool> ran{false};
        std::atomic<SubmitError> error{SubmitError::stopped};
        Delivery(const std::shared_ptr<Shared>& owner, std::uint64_t value)
            : receiver(owner), generation(value) {}
        ~Delivery() {
            if (!ran.load()) if (auto owner = receiver.lock()) owner->dropped(generation, error.load());
        }
    };
    struct Shared final {
        std::mutex mutex;
        TaskScope lifetime;
        std::function<Submission(Task)> submit;
        Task read;
        bool closed = false, queued = false, dirty = true;
        std::uint64_t generation = 0;
        SubmitError last = SubmitError::none;

        void dropped(std::uint64_t id, SubmitError error) {
            std::lock_guard lock(mutex);
            if (id == generation) { queued = false; dirty = true; last = error; }
        }
        SubmitError schedule(const std::shared_ptr<Shared>& self, bool change) {
            std::uint64_t id;
            {
                std::lock_guard lock(mutex);
                if (closed) return SubmitError::stopped;
                dirty = dirty || change;
                if (queued || !dirty) return last;
                queued = true; id = ++generation;
            }
            auto delivery = std::make_shared<Delivery>(self, id);
            Submission result;
            try {
                const std::weak_ptr<Shared> weak = self;
                result = submit([weak, delivery] {
                    delivery->ran = true;
                    if (auto owner = weak.lock()) owner->deliver(owner);
                });
            } catch (...) { result.error = SubmitError::invalid; }
            delivery->error = result.error;
            {
                std::lock_guard lock(mutex);
                if (id == generation) {
                    last = result.error;
                    if (!result) queued = false;
                }
            }
            return result.error;
        }
        void deliver(const std::shared_ptr<Shared>& self) {
            {
                std::lock_guard lock(mutex);
                if (closed) { queued = false; return; }
                dirty = false;
            }
            try { read(); }
            catch (...) { completed(self); throw; }
            completed(self);
        }
        void completed(const std::shared_ptr<Shared>& self) {
            {
                std::lock_guard lock(mutex); queued = false;
            }
            (void)schedule(self, false);
        }
    };
public:
    template<TaskExecutor Executor, class Handler>
    FutureWatcher(Future<T> future, Executor& executor, Handler handler)
        : state_(std::make_shared<Shared>()), notification_(std::make_shared<future_detail::Notification>()) {
        if (!future.valid()) throw std::invalid_argument("FutureWatcher requires a valid future");
        state_->submit = [&executor, sender = state_->lifetime.sender()](Task task) {
            return sender.post(executor, std::move(task));
        };
        state_->read = [future, handler = std::make_shared<Handler>(std::move(handler))] {
            std::invoke(*handler, future.snapshot());
        };
        const std::weak_ptr<Shared> weak = state_;
        notification_->notify = [weak] {
            if (auto owner = weak.lock()) (void)owner->schedule(owner, true);
        };
        if (!future.observe(notification_)) throw std::length_error("Future supports at most 16 live watchers");
        (void)state_->schedule(state_, true);
    }
    ~FutureWatcher() { (void)close(); }
    FutureWatcher(const FutureWatcher&) = delete;
    FutureWatcher& operator=(const FutureWatcher&) = delete;
    FutureWatcher(FutureWatcher&&) = delete;
    FutureWatcher& operator=(FutureWatcher&&) = delete;
    [[nodiscard]] SubmitError last_submission() const {
        std::lock_guard lock(state_->mutex); return state_->last;
    }
    [[nodiscard]] SubmitError retry() { return state_->schedule(state_, false); }
    [[nodiscard]] ThreadResult close() {
        if (state_->lifetime.is_current()) return std::unexpected(ThreadError{ThreadErrorCode::self_join, {}});
        {
            std::lock_guard lock(state_->mutex); state_->closed = true;
        }
        auto result = state_->lifetime.close();
        if (result) {
            notification_.reset();
            state_->read = {};
            // Keep the immutable weak admission endpoint until Shared retires;
            // an in-flight notification may still be returning from schedule().
        }
        return result;
    }
private:
    std::shared_ptr<Shared> state_;
    std::shared_ptr<future_detail::Notification> notification_;
};
} // namespace ii::runtime
