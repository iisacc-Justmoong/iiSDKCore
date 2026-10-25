#pragma once
#include "Runtime.h"
#include "TaskScope.h"
#include "HostLoop.h"
#include "../Core/Result/Result.h"
#include "../Core/Time/Deadline.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
#endif

namespace ii::runtime {
enum class OperationStatus { queued, running, succeeded, failed, cancelled, timed_out,
    owner_removed, executor_stopped, rejected };
enum class SideEffects { none, possible, committed };
enum class OperationWait { ready, timed_out, would_block };
struct OperationOptions final { core::Deadline deadline; Priority priority=Priority::normal; };
template<class T> struct OperationResult final {
    OperationStatus status=OperationStatus::queued;
    std::optional<T> value;
    std::optional<core::Error> error;
    SideEffects effects=SideEffects::none;
};
namespace operation_detail {
struct Control final {
    std::stop_source stop;
    std::stop_token owner;
    core::Deadline deadline;
    std::atomic<SideEffects> effects{SideEffects::none};
    [[nodiscard]] OperationStatus interruption() const {
        if (owner.stop_requested()) return OperationStatus::owner_removed;
        if (deadline.expired()) return OperationStatus::timed_out;
        if (stop.stop_requested()) return OperationStatus::cancelled;
        return OperationStatus::running;
    }
};
template<class T,class Work> struct Job;
}
// Borrowed for this invocation only. Cancellation is cooperative; a successful
// return still reports success if work already committed an external action.
class WorkContext final {
public:
    [[nodiscard]] OperationStatus interruption() const { return control_.interruption(); }
    [[nodiscard]] core::Result<void> checkpoint() const {
        if (interruption()!=OperationStatus::running)
            return core::fail(core::ErrorCode::cancelled,"Operation interrupted; inspect its final status and effects");
        return {};
    }
    [[nodiscard]] std::stop_token stop_token() const noexcept { return control_.stop.get_token(); }
    [[nodiscard]] std::stop_token owner_stop_token() const noexcept { return control_.owner; }
    [[nodiscard]] core::Deadline deadline() const noexcept { return control_.deadline; }
    void mark_effects(SideEffects effects) noexcept {
        auto current=control_.effects.load();
        while (current<effects && !control_.effects.compare_exchange_weak(current,effects)) {}
    }
    WorkContext(const WorkContext&)=delete;
    WorkContext& operator=(const WorkContext&)=delete;
private:
    template<class T,class Work> friend struct operation_detail::Job;
    explicit WorkContext(operation_detail::Control& control):control_(control){}
    operation_detail::Control& control_;
};
namespace operation_detail {
template<class T> struct State final {
    using Result=OperationResult<T>;
    using Snapshot=std::shared_ptr<const Result>;
    mutable std::mutex mutex;
    std::condition_variable changed;
    Control control;
    TaskToken token;
    SubmitError admission=SubmitError::none;
    OperationStatus phase=OperationStatus::queued;
    Snapshot result;
    bool observed=false;
    std::function<SubmitError(Snapshot)> completion;
    std::optional<SubmitError> completion_submission;
    void deliver(std::function<SubmitError(Snapshot)> callback,Snapshot value) {
        if (!callback) return;
        SubmitError submitted=SubmitError::invalid;
        try { submitted=callback(std::move(value)); } catch (...) {}
        std::lock_guard lock(mutex);
        completion_submission=submitted;
    }
    void finish(Result value) {
        { std::lock_guard lock(mutex); if (result) return; }
        auto snapshot=std::make_shared<const Result>(std::move(value));
        std::function<SubmitError(Snapshot)> callback;
        {
            std::lock_guard lock(mutex);
            if (result) return;
            phase=snapshot->status; result=snapshot;
            callback=std::move(completion);
        }
        changed.notify_all();
        deliver(std::move(callback),std::move(snapshot));
    }
};
template<class T,class Work> struct Job final {
    std::shared_ptr<State<T>> state;
    Work work;
    Job(std::shared_ptr<State<T>> value,Work callback):state(std::move(value)),work(std::move(callback)){}
    ~Job() {
        // Executors and TaskScope retire callbacks without executing them. The
        // last task capture settles admission-success work even on that path.
        if (auto reason=state->control.interruption();reason!=OperationStatus::running)
            state->finish({reason,{},{},state->control.effects.load()});
        else state->finish({OperationStatus::executor_stopped,{},{},state->control.effects.load()});
    }
    void execute() {
        {
            std::lock_guard lock(state->mutex);
            if (state->result) return;
            state->phase=OperationStatus::running;
        }
        WorkContext context(state->control);
        if (context.interruption()!=OperationStatus::running) {
            state->finish({context.interruption(),{},{},SideEffects::none}); return;
        }
        try {
            auto result=std::invoke(work,context);
            const auto effects=state->control.effects.load();
            if (result) state->finish({OperationStatus::succeeded,std::move(*result),{},effects});
            else {
                auto status=OperationStatus::failed;
                if (result.error().code==core::ErrorCode::cancelled) {
                    status=context.interruption();
                    if (status==OperationStatus::running) status=OperationStatus::cancelled;
                }
                state->finish({status,{},std::move(result.error()),effects});
            }
        } catch (...) {
            state->finish({OperationStatus::failed,{},core::Error{core::ErrorCode::failed,
                "Operation work threw",std::current_exception()},state->control.effects.load()});
        }
    }
};
}
template<class T> class Operation final {
public:
    using Snapshot=std::shared_ptr<const OperationResult<T>>;
    [[nodiscard]] SubmitError admission() const { std::lock_guard lock(state_->mutex); return state_->admission; }
    [[nodiscard]] OperationStatus status() const { std::lock_guard lock(state_->mutex); return state_->phase; }
    [[nodiscard]] Snapshot result() const { std::lock_guard lock(state_->mutex); return state_->result; }
    bool cancel() const {
        TaskToken token;
        { std::lock_guard lock(state_->mutex); if (state_->result) return false; token=state_->token; }
        const bool requested=state_->control.stop.request_stop();
        (void)token.cancel();
        return requested;
    }
    // A wait timeout only stops observation. It never marks work cancelled.
    // Pump hosts should poll result(); never block their dispatch thread.
    [[nodiscard]] OperationWait wait_for(Clock::duration duration) const {
        std::unique_lock lock(state_->mutex);
        if (state_->result) return OperationWait::ready;
        if (Runtime::is_worker_thread() || HostLoop::is_current_thread()) return OperationWait::would_block;
#if defined(__EMSCRIPTEN__)
        if (emscripten_is_main_browser_thread()) return OperationWait::would_block;
#endif
        return state_->changed.wait_for(lock,duration,[&]{return bool(state_->result);})
            ? OperationWait::ready : OperationWait::timed_out;
    }
    // Exactly one completion observer. Executor must outlive OperationOwner;
    // the sender endpoint safely suppresses a receiver removed before delivery.
    template<TaskExecutor Executor>
    bool then(Executor& executor,TaskScope::Sender receiver,
              std::function<void(Snapshot)> completion) const {
        if (!completion) return false;
        auto callback=[&executor,receiver,completion=std::move(completion)](Snapshot value) {
            return receiver.post(executor,[completion,value=std::move(value)]{completion(value);}).error;
        };
        return observe(std::move(callback));
    }
    // Own a copyable endpoint for asynchronous completion. Weak EventLoop/GUI
    // senders can go out of local scope before the worker finishes safely.
    template<TaskExecutor Executor> requires std::copy_constructible<Executor>
    bool then_on(Executor executor, TaskScope::Sender receiver,
                 std::function<void(Snapshot)> completion) const {
        if (!completion) return false;
        return observe([executor=std::move(executor),receiver,completion=std::move(completion)](Snapshot value) mutable {
            return receiver.post(executor,[completion,value=std::move(value)] { completion(value); }).error;
        });
    }
    [[nodiscard]] std::optional<SubmitError> completion_submission() const {
        std::lock_guard lock(state_->mutex); return state_->completion_submission;
    }
private:
    bool observe(std::function<SubmitError(Snapshot)> callback) const {
        Snapshot value;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->observed) return false;
            state_->observed=true;
            value=state_->result;
            if (!value) { state_->completion=std::move(callback); return true; }
        }
        state_->deliver(std::move(callback),std::move(value));
        return true;
    }
    friend class OperationOwner;
    explicit Operation(std::shared_ptr<operation_detail::State<T>> state):state_(std::move(state)){}
    std::shared_ptr<operation_detail::State<T>> state_;
};
// Instance lifetime for tracked work. Executors outlive the owner. close() first
// requests cooperative stop, then applies TaskScope's capture-retirement barrier.
class OperationOwner final {
public:
    OperationOwner()=default;
    ~OperationOwner() { (void)close(); }
    OperationOwner(const OperationOwner&)=delete;
    OperationOwner& operator=(const OperationOwner&)=delete;
    template<class T,TaskExecutor Executor,class Work>
        requires std::same_as<std::invoke_result_t<Work&,WorkContext&>,core::Result<T>> && (!std::is_void_v<T>)
    [[nodiscard]] Operation<T> submit(Executor& executor,Work work,OperationOptions options={}) const {
        auto state=std::make_shared<operation_detail::State<T>>();
        state->control.owner=stop_.get_token(); state->control.deadline=options.deadline;
        auto job=std::make_shared<operation_detail::Job<T,Work>>(state,std::move(work));
        const auto submitted=lifetime_.post(executor,[job]{job->execute();},options.priority);
        { std::lock_guard lock(state->mutex); state->admission=submitted.error; state->token=submitted.token; }
        if (!submitted) state->finish({OperationStatus::rejected,{},{},SideEffects::none});
        return Operation<T>(std::move(state));
    }
    void request_close() const {
        stop_.request_stop(); lifetime_.request_close();
    }
    [[nodiscard]] ThreadResult close() const {
        if (lifetime_.is_current()) return std::unexpected(ThreadError{ThreadErrorCode::self_join,{}});
        request_close(); return lifetime_.close();
    }
private:
    mutable std::stop_source stop_;
    TaskScope lifetime_;
};
} // namespace ii::runtime
