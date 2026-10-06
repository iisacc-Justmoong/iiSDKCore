#pragma once
#include "Thread.h"
#include <concepts>

namespace ii::runtime {
// Owns submitted callbacks until their captures have been destroyed. Closing
// cancels queued work, waits for active work, then retires owned resources.
class TaskScope final {
public:
    explicit TaskScope(Task cleanup = {});
    ~TaskScope();
    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;
    TaskScope(TaskScope&&) = delete;
    TaskScope& operator=(TaskScope&&) = delete;

    template<class Executor>
    [[nodiscard]] Submission post(Executor& executor, Task task,
                                  Priority priority = Priority::normal) const
        requires requires(Executor& target, Task work) {
            { target.post(std::move(work), Priority::normal) } -> std::same_as<Submission>;
        } {
        if (!task) return {{}, SubmitError::invalid};
        const auto job = prepare(std::move(task));
        if (!job) return {{}, SubmitError::stopped};
        const auto submitted = executor.post(callback(job), priority);
        registered(job, submitted.token);
        return submitted;
    }
    // Timed one-shot work has the same removal barrier as immediate work.
    [[nodiscard]] Submission at(EventLoop& executor, Clock::time_point deadline, Task task,
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
    [[nodiscard]] ThreadResult close() const;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] bool closed() const;
private:
    struct Impl;
    struct Job;
    std::shared_ptr<Impl> impl_;
    std::shared_ptr<Job> prepare(Task) const;
    static Task callback(const std::shared_ptr<Job>&);
    static void registered(const std::shared_ptr<Job>&, TaskToken);
};
} // namespace ii::runtime
