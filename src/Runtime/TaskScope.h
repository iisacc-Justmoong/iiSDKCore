#pragma once
#include "Thread.h"
#include <concepts>

namespace ii::runtime {
// Owns submitted callbacks until their captures have been destroyed. Closing
// cancels queued work, waits for active work, then retires owned resources.
class TaskScope final {
    struct Impl;
    struct Job;
public:
    // A copyable admission endpoint. It never keeps the owner alive and safely
    // rejects deliveries after the owner's close/removal barrier.
    class Sender final {
    public:
        template<TaskExecutor Executor>
        [[nodiscard]] Submission post(Executor& executor, Task task,
                                      Priority priority=Priority::normal) const {
            if (!task) return {{},SubmitError::invalid};
            const auto owner=owner_.lock();
            if (!owner) return {{},SubmitError::stopped};
            const auto job=TaskScope::prepare(owner,std::move(task));
            if (!job) return {{},SubmitError::stopped};
            const auto submitted=executor.post(TaskScope::callback(job),priority);
            TaskScope::registered(job,submitted.token);
            return submitted;
        }
        template<TimedTaskExecutor Executor>
        [[nodiscard]] Submission at(Executor& executor, Clock::time_point deadline, Task task,
                                    Priority priority=Priority::normal) const {
            if (!task) return {{},SubmitError::invalid};
            const auto owner=owner_.lock();
            if (!owner) return {{},SubmitError::stopped};
            const auto job=TaskScope::prepare(owner,std::move(task));
            if (!job) return {{},SubmitError::stopped};
            const auto submitted=executor.at(deadline,TaskScope::callback(job),priority);
            TaskScope::registered(job,submitted.token);
            return submitted;
        }
    private:
        friend class TaskScope;
        explicit Sender(const std::shared_ptr<Impl>& owner):owner_(owner){}
        std::weak_ptr<Impl> owner_;
    };
    explicit TaskScope(Task cleanup = {});
    ~TaskScope();
    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;
    TaskScope(TaskScope&&) = delete;
    TaskScope& operator=(TaskScope&&) = delete;

    template<class Executor>
    [[nodiscard]] Submission post(Executor& executor, Task task,
                                  Priority priority = Priority::normal) const
        requires TaskExecutor<Executor> {
        if (!task) return {{}, SubmitError::invalid};
        const auto job = prepare(std::move(task));
        if (!job) return {{}, SubmitError::stopped};
        const auto submitted = executor.post(callback(job), priority);
        registered(job, submitted.token);
        return submitted;
    }
    // Timed one-shot work has the same removal barrier as immediate work.
    template<TimedTaskExecutor Executor>
    [[nodiscard]] Submission at(Executor& executor, Clock::time_point deadline, Task task,
                                Priority priority = Priority::normal) const {
        if (!task) return {{}, SubmitError::invalid};
        const auto job = prepare(std::move(task));
        if (!job) return {{}, SubmitError::stopped};
        const auto submitted = executor.at(deadline, callback(job), priority);
        registered(job, submitted.token);
        return submitted;
    }
    // A synchronous operation participates in the same lifetime boundary.
    [[nodiscard]] SubmitError run(Task task) const;
    // Stop admission and cancel queued work without waiting for active work.
    // Capture destructors still run synchronously; close() is the final barrier.
    void request_close() const;
    [[nodiscard]] ThreadResult close() const;
    // Close admission/cancel queued work and finish only if all callbacks,
    // captures and executor slots have retired. Never wait for active work.
    [[nodiscard]] ThreadResult try_close() const;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] bool closed() const;
    [[nodiscard]] Sender sender() const { return Sender(impl_); }
private:
    std::shared_ptr<Impl> impl_;
    std::shared_ptr<Job> prepare(Task) const;
    static std::shared_ptr<Job> prepare(const std::shared_ptr<Impl>&,Task);
    static Task callback(const std::shared_ptr<Job>&);
    static void registered(const std::shared_ptr<Job>&, TaskToken);
    [[nodiscard]] ThreadResult finish(bool wait) const;
};
} // namespace ii::runtime
