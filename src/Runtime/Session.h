#pragma once

#include "ObjectOwner.h"
#include "TaskScope.h"
#include <memory>
#include <stop_token>

namespace ii::runtime {
class Runtime;
enum class SessionState { active, closing, closed };

// Session admission, tasks and owned objects end together. The Runtime closes
// any surviving sessions after it drains its workers and before its own objects.
class Session final {
    struct State;
public:
    explicit Session(Runtime& runtime);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    [[nodiscard]] ObjectOwner& objects() const;
    [[nodiscard]] TaskScope::Sender sender() const;
    template<TaskExecutor Executor>
    [[nodiscard]] Submission post(Executor& executor, Task task, Priority priority=Priority::normal) const {
        return sender().post(executor,std::move(task),priority);
    }
    template<TimedTaskExecutor Executor>
    [[nodiscard]] Submission at(Executor& executor, Clock::time_point deadline, Task task,
                                 Priority priority=Priority::normal) const {
        return sender().at(executor,deadline,std::move(task),priority);
    }
    [[nodiscard]] std::stop_token stop_token() const noexcept;
    void request_close() const;
    [[nodiscard]] ThreadResult close() const;
    [[nodiscard]] ThreadResult try_close() const;
    [[nodiscard]] bool closed() const;
    [[nodiscard]] SessionState state() const noexcept;
private:
    friend class Runtime;
    static ThreadResult finish(const std::shared_ptr<State>& state, bool wait);
    static bool is_current(const std::shared_ptr<State>& state) noexcept;
    std::shared_ptr<State> state_;
};
} // namespace ii::runtime
