#pragma once
#include "../Result/Result.h"
#include "../../Runtime/Runtime.h"
#include <mutex>

namespace ii::core {
// Nonblocking completion observation. Runtime owns scheduling and worker waits.
class TaskGroup final {
public:
    explicit TaskGroup(runtime::Runtime& runtime) : runtime_(runtime),state_(std::make_shared<State>()) {}
    TaskGroup(const TaskGroup&)=delete;
    TaskGroup& operator=(const TaskGroup&)=delete;
    [[nodiscard]] runtime::Submission post(runtime::Task task) {
        if (!task) return {{},runtime::SubmitError::invalid};
        auto state=state_;
        {
            std::lock_guard lock(state->mutex);
            if (state->closed) return {{},runtime::SubmitError::stopped};
            ++state->pending;
        }
        std::shared_ptr<Ticket> ticket;
        try {
            ticket=std::make_shared<Ticket>(state);
            auto submission=runtime_.post([state,ticket,task=std::move(task)]() mutable {
                bool cancelled;
                { std::lock_guard lock(state->mutex); cancelled=state->cancelled; }
                std::exception_ptr error;
                if (!cancelled) try { task(); } catch (...) { error=std::current_exception(); }
                task=nullptr; // captured resources retire outside the state lock
                ticket->finish(cancelled,error);
            });
            if (!submission) ticket->finish(false);
            return submission;
        } catch (...) {
            if (ticket) ticket->finish(false);
            else { std::lock_guard lock(state->mutex); --state->pending; }
            throw;
        }
    }
    void close() { std::lock_guard lock(state_->mutex); state_->closed=true; }
    void cancel() {
        std::lock_guard lock(state_->mutex); state_->closed=true; state_->cancelled=true;
    }
    [[nodiscard]] std::size_t pending() const { std::lock_guard lock(state_->mutex); return state_->pending; }
    [[nodiscard]] std::optional<Result<void>> result() const {
        std::lock_guard lock(state_->mutex);
        if (!state_->closed || state_->pending) return std::nullopt;
        if (state_->failure) return Result<void>(fail(ErrorCode::failed,"Task group member failed",state_->failure));
        if (state_->cancelled || state_->discarded) return Result<void>(fail(ErrorCode::cancelled,"Task group work cancelled"));
        return Result<void>{};
    }
private:
    struct State {
        std::mutex mutex;
        std::size_t pending=0;
        bool closed=false,cancelled=false,discarded=false;
        std::exception_ptr failure;
    };
    // Runtime cancellation may destroy a queued callable without invoking it.
    // The last ticket owner settles that work, including after group destruction.
    struct Ticket {
        std::shared_ptr<State> state;
        bool finished=false;
        explicit Ticket(std::shared_ptr<State> value):state(std::move(value)) {}
        ~Ticket() { finish(true); }
        void finish(bool cancelled,std::exception_ptr error={}) {
            std::lock_guard lock(state->mutex);
            if (finished) return;
            if (error && !state->failure) state->failure=std::move(error);
            state->discarded|=cancelled;
            --state->pending;
            finished=true;
        }
    };
    runtime::Runtime& runtime_;
    std::shared_ptr<State> state_;
};
} // namespace ii::core
