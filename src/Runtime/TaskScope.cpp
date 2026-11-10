#include "TaskScope.h"
#include "Runtime.h"
#include "HostLoop.h"
#include <algorithm>
#include <condition_variable>
#include <list>
#include <mutex>
#include <vector>
#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
#endif

namespace ii::runtime {
namespace {
// A stack also detects closing an outer scope from a nested callback/destructor.
struct Activity {
    const void* owner;
    Activity* previous;
    static thread_local Activity* current;
    explicit Activity(const void* value) : owner(value), previous(current) { current=this; }
    ~Activity() { current=previous; }
    static bool contains(const void* value) {
        for (auto* entry=current;entry;entry=entry->previous) if (entry->owner==value) return true;
        return false;
    }
};
thread_local Activity* Activity::current=nullptr;
bool browser_host() {
#if defined(__EMSCRIPTEN__)
    return emscripten_is_main_browser_thread();
#else
    return false;
#endif
}
ThreadResult error(ThreadErrorCode code) { return std::unexpected(ThreadError{code,{}}); }
}
struct TaskScope::Impl {
    struct Entry { Job* job; TaskToken token; bool registered=false,cancelling=false; };
    std::mutex mutex;
    std::condition_variable changed;
    std::list<Entry> jobs;
    std::list<Entry> retired;
    std::size_t active=0;
    bool closed=false, cleaning=false, cleaned=false;
    Task cleanup;
    std::exception_ptr failure;
    explicit Impl(Task finalizer) : cleanup(std::move(finalizer)) {}
    void prune_retired() {
        // Entries enter this list once, in retirement order. Active jobs are
        // never revisited when unrelated work is admitted.
        while (!retired.empty() && !retired.front().token.pending()) retired.pop_front();
    }
    ThreadResult result() const {
        if (failure) return std::unexpected(ThreadError{ThreadErrorCode::work_failed,failure});
        return {};
    }
};
struct TaskScope::Job {
    std::shared_ptr<Impl> owner;
    Task task;
    bool listed=false;
    std::list<Impl::Entry>::iterator entry;
    Job(std::shared_ptr<Impl> state,Task work) : owner(std::move(state)), task(std::move(work)) {}
    ~Job() {
        Activity retiring(owner.get());
        task=nullptr; // retain the accounting entry until every capture is gone
        if (listed) {
            std::lock_guard lock(owner->mutex);
            entry->job=nullptr;
            if (!entry->token.pending()) owner->jobs.erase(entry);
            else owner->retired.splice(owner->retired.end(),owner->jobs,entry);
            if (owner->closed) owner->changed.notify_all();
        }
    }
    void execute() {
        {
            std::lock_guard lock(owner->mutex);
            if (owner->closed) return;
            ++owner->active;
        }
        struct Finish {
            Impl& state;
            ~Finish() { std::lock_guard lock(state.mutex); --state.active; if (state.closed) state.changed.notify_all(); }
        } finish{*owner};
        Activity executing(owner.get());
        task();
    }
};
TaskScope::TaskScope(Task cleanup) : impl_(std::make_shared<Impl>(std::move(cleanup))) {}
TaskScope::~TaskScope() {
    const auto result=close();
    if (!result && result.error().code!=ThreadErrorCode::work_failed) std::terminate();
}
std::shared_ptr<TaskScope::Job> TaskScope::prepare(Task task) const {
    return prepare(impl_,std::move(task));
}
std::shared_ptr<TaskScope::Job> TaskScope::prepare(const std::shared_ptr<Impl>& owner,Task task) {
    auto job=std::make_shared<Job>(owner,std::move(task));
    std::lock_guard lock(owner->mutex);
    if (owner->closed) return {};
    owner->prune_retired();
    owner->jobs.push_back({job.get(),{},false,false});
    job->entry=std::prev(owner->jobs.end()); job->listed=true;
    return job;
}
Task TaskScope::callback(const std::shared_ptr<Job>& job) { return [job]{job->execute();}; }
void TaskScope::registered(const std::shared_ptr<Job>& job,TaskToken token) {
    bool cancel=false;
    {
        std::lock_guard lock(job->owner->mutex);
        job->entry->token=token; job->entry->registered=true;
        cancel=job->owner->closed && !job->entry->cancelling;
        if(cancel) job->entry->cancelling=true;
        if (job->owner->closed) job->owner->changed.notify_all();
    }
    // request_close may have raced the executor's admission. Complete that
    // cancellation here, outside the scope lock, once the token becomes known.
    if(cancel) (void)token.cancel();
}
SubmitError TaskScope::run(Task task) const {
    if (!task) return SubmitError::invalid;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->closed) return SubmitError::stopped;
        ++impl_->active;
    }
    struct Finish {
        Impl& state;
        ~Finish() { std::lock_guard lock(state.mutex); --state.active; if (state.closed) state.changed.notify_all(); }
    } finish{*impl_};
    Activity executing(impl_.get());
    try { task(); } catch (...) { task=nullptr; throw; }
    task=nullptr;
    return SubmitError::none;
}
void TaskScope::request_close() const {
    std::vector<TaskToken> tokens;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->closed) return;
        tokens.reserve(impl_->jobs.size());
        impl_->closed=true;
        for(auto& entry:impl_->jobs) if(entry.registered && !entry.cancelling) {
            tokens.push_back(entry.token);
            entry.cancelling=true;
        }
    }
    // Cancellation can destroy captures and reenter this scope.
    for(const auto& token:tokens) (void)token.cancel();
}
ThreadResult TaskScope::close() const { return finish(true); }
ThreadResult TaskScope::try_close() const { return finish(false); }
ThreadResult TaskScope::finish(bool wait) const {
    if (is_current()) return error(ThreadErrorCode::self_join);
    request_close();
    std::unique_lock lock(impl_->mutex);
    for (;;) {
        if (impl_->cleaned) return impl_->result();
        impl_->prune_retired();
        if (impl_->jobs.empty() && impl_->retired.empty() && !impl_->active && !impl_->cleaning) break;
        if (!wait || browser_host() || Runtime::is_worker_thread() || HostLoop::is_current_thread()) return error(ThreadErrorCode::would_block);
        const bool retiring_only=impl_->jobs.empty() && !impl_->retired.empty() && !impl_->active && !impl_->cleaning;
        if (retiring_only) {
            const auto token=impl_->retired.front().token;
            lock.unlock();
            const auto waited=token.wait();
            lock.lock();
            if (!waited) return error(ThreadErrorCode::would_block);
        } else impl_->changed.wait(lock);
    }
    impl_->cleaning=true;
    Task cleanup;
    cleanup.swap(impl_->cleanup);
    lock.unlock();
    std::exception_ptr failure;
    {
        Activity retiring(impl_.get());
        try { if (cleanup) cleanup(); } catch (...) { failure=std::current_exception(); }
        cleanup=nullptr;
    }
    lock.lock();
    impl_->failure=failure; impl_->cleaning=false; impl_->cleaned=true;
    impl_->changed.notify_all();
    return impl_->result();
}
bool TaskScope::is_current() const noexcept { return Activity::contains(impl_.get()); }
bool TaskScope::closed() const { std::lock_guard lock(impl_->mutex); return impl_->closed; }
} // namespace ii::runtime
