#include "Thread.h"

#include <condition_variable>
#include <mutex>
#include <optional>

#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
#endif

namespace ii::runtime {
namespace {
ThreadResult failure(ThreadErrorCode code, std::exception_ptr cause = {}) {
    return std::unexpected(ThreadError{code, cause});
}
bool host_thread() noexcept {
#if defined(__EMSCRIPTEN__)
    return emscripten_is_main_browser_thread();
#else
    return false;
#endif
}
}

struct ThreadContext::State {
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool stop = false;
    ThreadState status = ThreadState::idle;
    Thread::Id id;
    std::exception_ptr error;
    std::optional<std::reference_wrapper<EventLoop>> loop;
};

bool ThreadContext::stop_requested() const {
    std::lock_guard lock(state_.mutex);
    return state_.stop;
}
bool ThreadContext::wait_for(Clock::duration duration) const {
    std::unique_lock lock(state_.mutex);
    return state_.changed.wait_for(lock, duration, [&] { return state_.stop; });
}

struct Thread::Impl {
    mutable std::mutex owner;
    ThreadContext::State control;
    bool has_worker = false;
    bool joining = false;
    Work pending;
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    std::thread worker;
#endif

    ThreadResult launch(Work work, std::optional<std::reference_wrapper<EventLoop>> loop) {
        if (!Thread::supported()) return failure(ThreadErrorCode::unsupported);
        if (host_thread()) return failure(ThreadErrorCode::would_block);
        if (!work) return failure(ThreadErrorCode::invalid_work);
        std::exception_ptr previous_failure;
        std::unique_lock owner_lock(owner);
        if (has_worker) return failure(ThreadErrorCode::busy);
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
        // Publish the new run before its callback may inspect/reenter Thread.
        // The control block itself is reused across joined runs.
        std::unique_lock state_lock(control.mutex);
        pending.swap(work);
        try {
            worker = std::thread([this] {
                Work task;
                { std::lock_guard started(control.mutex); task.swap(pending); }
                ThreadContext context(control);
                std::exception_ptr error;
                try { task(context); } catch (...) { error = std::current_exception(); }
                task = nullptr; // release captures before publishing completion
                std::lock_guard finished(control.mutex);
                if (error && control.loop) control.loop->get().request_stop();
                control.error = error;
                control.status = error ? ThreadState::failed : ThreadState::completed;
                control.loop.reset();
            });
        } catch (...) {
            // Captures are destroyed only after our locks have been released.
            pending.swap(work);
            return failure(ThreadErrorCode::start_failed, std::current_exception());
        }
        control.stop = false;
        control.status = ThreadState::running;
        control.id = worker.get_id();
        std::swap(previous_failure, control.error);
        control.loop = loop;
        has_worker = true;
        return {};
#else
        (void)loop;
        return failure(ThreadErrorCode::unsupported);
#endif
    }

    ThreadResult result() const {
        std::lock_guard lock(control.mutex);
        if (control.error) return failure(ThreadErrorCode::work_failed, control.error);
        return {};
    }
};

Thread::Thread() : impl_(std::make_unique<Impl>()) {}
Thread::~Thread() {
    request_stop();
    auto result = join();
    // A running object cannot destroy its own storage or abandon its worker.
    // Work failures are available via explicit join(), not thrown by destruction.
    if (!result && result.error().code != ThreadErrorCode::work_failed) std::terminate();
}
ThreadResult Thread::start(Work work) { return impl_->launch(std::move(work), {}); }
ThreadResult Thread::start(EventLoop& loop, PumpBudget budget) {
    return impl_->launch([&loop, budget](ThreadContext&) { loop.run(budget); }, std::ref(loop));
}
bool Thread::request_stop() {
    auto& control = impl_->control;
    std::lock_guard lock(control.mutex);
    if (control.stop || (control.status != ThreadState::running && control.status != ThreadState::stop_requested))
        return false;
    control.stop = true;
    control.status = ThreadState::stop_requested;
    // Loop wake contains no user callback. Keeping this lock prevents join from
    // completing while a concurrent stop still accesses the borrowed loop.
    if (control.loop) control.loop->get().request_stop();
    control.changed.notify_all();
    return true;
}
ThreadResult Thread::join() {
    std::unique_lock lock(impl_->owner);
    if (!impl_->has_worker) return impl_->result();
    if (id() == current_id()) return failure(ThreadErrorCode::self_join);
    if (host_thread()) return failure(ThreadErrorCode::would_block);
    if (impl_->joining) return failure(ThreadErrorCode::busy);
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    impl_->joining = true;
    lock.unlock();
    try {
        impl_->worker.join();
    } catch (...) {
        lock.lock();
        impl_->joining = false;
        return failure(ThreadErrorCode::join_failed, std::current_exception());
    }
    lock.lock();
    impl_->joining = false;
    impl_->has_worker = false;
#endif
    return impl_->result();
}
bool Thread::joinable() const { std::lock_guard lock(impl_->owner); return impl_->has_worker; }
ThreadState Thread::state() const { std::lock_guard lock(impl_->control.mutex); return impl_->control.status; }
Thread::Id Thread::id() const { std::lock_guard lock(impl_->control.mutex); return impl_->control.id; }
Thread::Id Thread::current_id() noexcept { return std::this_thread::get_id(); }
bool Thread::supported() noexcept {
#if defined(__EMSCRIPTEN__)
    return emscripten_has_threading_support();
#else
    return true;
#endif
}

} // namespace ii::runtime
