#include <iiSDKCore/Runtime/Runtime.h>
#include <iiSDKCore/Runtime/Session.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace ii::runtime;
using namespace std::chrono_literals;

namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Trace {
    std::mutex mutex;
    std::vector<int> values;
    void add(int value) { std::lock_guard lock(mutex); values.push_back(value); }
    std::vector<int> copy() { std::lock_guard lock(mutex); return values; }
};
struct Resource {
    Trace& trace;
    int value;
    Resource(Trace& t, int n):trace(t),value(n) {}
    ~Resource() { trace.add(value); }
};
struct OnDestroy {
    std::function<void()> callback;
    explicit OnDestroy(std::function<void()> action):callback(std::move(action)) {}
    ~OnDestroy() { callback(); }
};
struct SelfSharing : std::enable_shared_from_this<SelfSharing> {
    bool& destroyed;
    explicit SelfSharing(bool& value):destroyed(value) {}
    ~SelfSharing() { destroyed=true; }
};
struct LeaseHolder {
    std::shared_ptr<int> lease;
    explicit LeaseHolder(std::shared_ptr<int> value):lease(std::move(value)) {}
};
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered=false, released=false;
    void wait_inside() {
        std::unique_lock lock(mutex); entered=true; changed.notify_all();
        if (!changed.wait_for(lock,5s,[&]{ return released; })) throw std::runtime_error("gate timed out");
    }
    void wait_entered() {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock,5s,[&]{ return entered; })) throw std::runtime_error("entry timed out");
    }
    void release() { std::lock_guard lock(mutex); released=true; changed.notify_all(); }
};
struct ReleaseGate { Gate& gate; ~ReleaseGate(){gate.release();} };
struct JoiningThread final {
    std::thread thread;
    JoiningThread() = default;
    template<class F> explicit JoiningThread(F&& work):thread(std::forward<F>(work)) {}
    ~JoiningThread() { if (thread.joinable()) thread.join(); }
    template<class F> void start(F&& work) { thread=std::thread(std::forward<F>(work)); }
    void join() { if (thread.joinable()) thread.join(); }
};

void independent_lifetimes() {
    Trace trace;
    Runtime runtime({.workers=1,.capacity=8});
    Session session(runtime);
    check(session.state()==SessionState::active,"new session starts active");
    auto lease=std::make_shared<int>(42); std::weak_ptr<int> released=lease;
    auto holder=session.objects().emplace<LeaseHolder>(std::move(lease));
    auto root=runtime.objects().emplace<Resource>(trace,1);
    auto a=session.objects().emplace<Resource>(trace,2);
    auto b=session.objects().adopt(std::make_unique<Resource>(trace,3));
    check(holder && root && a && b,"objects are admitted to their owners");
    check(session.objects().size()==3 && runtime.objects().size()==1,"scope counts are independent");
    check(bool(session.close()),"session closes");
    check(trace.copy()==std::vector<int>({3,2}),"session destroys in reverse attachment order");
    check(released.expired(),"destruction releases resources owned by a session object");
    check(a->with([](Resource&) {})==SubmitError::stopped,"old session handle cannot access an object");
    int observed=0;
    check(root->with([&](Resource& r){observed=r.value;})==SubmitError::none && observed==1,
          "runtime object remains available after session closes");
    check(bool(session.close()) && bool(runtime.join()),"close and runtime shutdown are idempotent");
    check(trace.copy()==std::vector<int>({3,2,1}),"runtime object is destroyed after the session");
    check(root->with([](Resource&) {})==SubmitError::stopped,"runtime handle stops after join");
    auto rejected=runtime.objects().emplace<Resource>(trace,4);
    check(!rejected && rejected.error()==ObjectError::closed,"closed runtime rejects objects");
}

void session_outlives_runtime() {
    Trace trace;
    auto runtime=std::make_unique<Runtime>(RuntimeOptions{.workers=1,.capacity=8});
    auto session=runtime->create_session();
    auto a=session->objects().emplace<Resource>(trace,10);
    auto root=runtime->objects().emplace<Resource>(trace,11);
    check(a && root,"objects accepted before runtime destruction");
    runtime.reset();
    check(trace.copy()==std::vector<int>({10,11}),"runtime destructor closes live sessions before its objects");
    check(session->closed() && a->with([](Resource&){})==SubmitError::stopped,
          "session can safely outlive runtime but cannot revive objects");
    check(bool(session->close()),"session remains idempotently closed");
}

void active_access_and_close() {
    Trace trace; Runtime runtime({.workers=1,.capacity=8}); Session session(runtime); Gate gate;
    auto object=session.objects().emplace<Resource>(trace,20);
    check(bool(object),"active resource accepted");
    std::atomic<SubmitError> accessed=SubmitError::invalid;
    JoiningThread reader([&]{accessed=object->with([&](Resource& r){check(r.value==20,"valid access");gate.wait_inside();});});
    ReleaseGate cleanup{gate};
    gate.wait_entered();
    session.request_close();
    check(session.state()==SessionState::closing,"close request enters closing state");
    auto before=session.try_close();
    check(!before && before.error().code==ThreadErrorCode::would_block,"try_close never waits for an active access");
    check(trace.copy().empty(),"active object is not destroyed");
    check(object->with([](Resource&){})==SubmitError::stopped,"closing rejects new access");
    gate.release(); reader.join();
    check(accessed==SubmitError::none && bool(session.close()),"close succeeds after active access returns");
    check(session.state()==SessionState::closed,"session reaches closed after cleanup");
    check(trace.copy()==std::vector<int>({20}),"object is released once after the access");
}

void tasks_and_external_executor() {
    Trace trace; Runtime runtime({.workers=1,.capacity=8}); Session session(runtime); Gate active;
    auto object=session.objects().emplace<Resource>(trace,30);
    check(bool(object),"session task resource accepted");
    check(bool(session.post(runtime,[&]{
        (void)object->with([&](Resource&){active.wait_inside();});
    })),"session task accepted");
    active.wait_entered();
    auto capture=std::make_shared<int>(4); std::weak_ptr<int> weak=capture;
    check(bool(session.post(runtime,[capture]{})),"queued session task accepted"); capture.reset();
    session.request_close();
    check(session.stop_token().stop_requested() && weak.expired(),"close requests stop and retires queued captures");
    check(session.post(runtime,[]{ }).error==SubmitError::stopped,"closed session rejects new jobs");
    auto pending=session.try_close();
    check(!pending && pending.error().code==ThreadErrorCode::would_block && trace.copy().empty(),"active task keeps object alive");
    active.release();
    check(bool(session.close()) && trace.copy()==std::vector<int>({30}),"close drains running task before object release");
    check(bool(runtime.wait_idle()),"worker capacity is returned");

    Session external(runtime); EventLoop loop(2);
    auto pending_capture=std::make_shared<int>(5); std::weak_ptr<int> pending_weak=pending_capture;
    check(bool(external.post(loop,[pending_capture]{})),"session admits event-loop task"); pending_capture.reset();
    auto timer_capture=std::make_shared<int>(6); std::weak_ptr<int> timer_weak=timer_capture;
    check(bool(external.at(loop,Clock::now()+1h,[timer_capture]{})),"session admits a timer on another executor");
    timer_capture.reset();
    check(bool(runtime.join()),"runtime joins with a live session");
    check(external.closed() && pending_weak.expired() && timer_weak.expired() && loop.stats().pending==0,
          "runtime closes session and removes tasks and timers from other executors");
}

void individual_removal_and_reentry() {
    Trace trace; Runtime runtime({.workers=1,.capacity=4}); Session first(runtime), second(runtime);
    auto object=first.objects().emplace<Resource>(trace,40);
    check(bool(object),"removable object accepted");
    auto wrong=second.objects().erase(*object);
    check(!wrong && wrong.error().code==ThreadErrorCode::invalid_work,"other owner cannot remove object");
    check(object->with([&](Resource&){
        auto removal=first.objects().erase(*object);
        check(!removal && removal.error().code==ThreadErrorCode::self_join,"self-removal is rejected");
        auto closing=first.close();
        check(!closing && closing.error().code==ThreadErrorCode::self_join,"session cannot close from its own object access");
        auto joined=runtime.join();
        check(!joined && joined.error().code==ThreadErrorCode::self_join && runtime.stats().state==RuntimeState::running,
              "runtime join from session access is rejected before shutdown");
    })==SubmitError::none,"reentrant access returned");
    bool propagated=false;
    try { (void)object->with([](Resource&){throw std::runtime_error("access failure");}); }
    catch (const std::runtime_error&) { propagated=true; }
    check(propagated && !first.closed(),"an exception releases access accounting without closing the session");
    check(!first.closed() && bool(first.objects().erase(*object)),"external erase destroys the object");
    check(trace.copy()==std::vector<int>({40}) && object->with([](Resource&){})==SubmitError::stopped,
          "erase invalidates its non-owning handle");
    auto invalid=first.objects().adopt(std::unique_ptr<Resource>{});
    check(!invalid && invalid.error()==ObjectError::invalid_object,"null ownership rejected");
    bool destroyed=false, can_escape=true;
    auto sharing=first.objects().emplace<SelfSharing>(destroyed);
    check(bool(sharing),"self-sharing type accepted");
    check(sharing->with([&](SelfSharing& value){can_escape=!value.weak_from_this().expired();})==SubmitError::none && !can_escape,
          "owner never creates an external shared ownership path");
    check(bool(first.close()) && destroyed,"self-sharing object is destroyed at session close");
    bool rejected_destroyed=false;
    auto rejected=first.objects().adopt(std::make_unique<SelfSharing>(rejected_destroyed));
    check(!rejected && rejected.error()==ObjectError::closed && rejected_destroyed,
          "failed adoption still releases the transferred object");
}

void reentrant_runtime_and_failure_cleanup() {
    Trace trace;
    Runtime runtime({.workers=1,.capacity=8}); Session first(runtime),second(runtime);
    auto a=first.objects().emplace<Resource>(trace,60);
    auto b=second.objects().emplace<Resource>(trace,61);
    auto root=runtime.objects().emplace<Resource>(trace,62);
    check(a && b && root,"root and session resources accepted");
    check(root->with([&](Resource&){
        auto result=runtime.join();
        check(!result && result.error().code==ThreadErrorCode::self_join && runtime.stats().state==RuntimeState::running,
              "runtime join from its own object is rejected before shutdown");
    })==SubmitError::none,"root access completed");
    EventLoop loop(2); bool rejected=false;
    check(bool(first.post(loop,[&]{auto result=runtime.join(); rejected=!result && result.error().code==ThreadErrorCode::self_join;})),
          "session event loop callback accepted");
    check(loop.poll({2,1s})==1 && rejected && runtime.stats().state==RuntimeState::running,
          "runtime join from a session callback is rejected");
    check(bool(runtime.join()) && trace.copy()==std::vector<int>({61,60,62}),
          "runtime closes sessions in reverse creation order, then its own objects");

    Trace failed_trace; Runtime failed({.workers=1,.capacity=4}); Session failing(failed);
    check(bool(failing.objects().emplace<Resource>(failed_trace,63)),"failing runtime object accepted");
    check(bool(failing.post(failed,[]{throw std::runtime_error("expected failure");})),"failing task accepted");
    auto result=failed.join();
    check(!result && result.error().code==ThreadErrorCode::work_failed && failing.closed() &&
          failed_trace.copy()==std::vector<int>({63}),"work failure does not skip lifetime cleanup");
}

void destructor_reentry() {
    Runtime runtime({.workers=1,.capacity=4}); Session session(runtime);
    bool observed=false;
    auto object=session.objects().emplace<OnDestroy>([&]{
        auto closing=session.close();
        auto joined=runtime.join();
        observed=!closing && closing.error().code==ThreadErrorCode::self_join &&
            !joined && joined.error().code==ThreadErrorCode::self_join &&
            session.objects().size()==1 && runtime.stats().state==RuntimeState::running;
    });
    check(bool(object),"reentrant destructor object accepted");
    check(bool(session.close()) && observed && session.objects().size()==0,
          "destructors run outside owner locks and cannot join their own cleanup");
}

void stop_callback_releases_session() {
    Trace trace; Runtime runtime({.workers=1,.capacity=4});
    for (int mode=0;mode<2;++mode) {
        auto session=runtime.create_session();
        check(bool(session->objects().emplace<Resource>(trace,90+mode)),"stop-callback resource accepted");
        std::stop_callback callback(session->stop_token(),[&]{session.reset();});
        auto* borrowed=session.get();
        if (mode==0) borrowed->request_close();
        else check(bool(borrowed->close()),"close survives the last owner being released by a stop callback");
        check(!session && trace.copy().size()==static_cast<std::size_t>(mode+1),
              "stop callback can release the session without losing resource cleanup");
    }
}

void foreign_worker_can_retry_shutdown() {
    Trace trace; Runtime owner({.workers=1,.capacity=4}), other({.workers=1,.capacity=4});
    Session session(owner); Gate gate;
    auto object=session.objects().emplace<Resource>(trace,70);
    check(bool(object),"foreign-worker resource accepted");
    JoiningThread reader([&]{(void)object->with([&](Resource&){gate.wait_inside();});});
    ReleaseGate cleanup{gate}; gate.wait_entered();
    std::atomic<bool> rejected=false;
    check(bool(other.post([&]{auto result=owner.join(); rejected=!result && result.error().code==ThreadErrorCode::would_block;})),
          "foreign worker shutdown accepted");
    check(bool(other.wait_idle()) && rejected && session.state()==SessionState::closing && trace.copy().empty(),
          "foreign worker reports would_block without premature destruction");
    gate.release(); reader.join();
    check(bool(owner.join()) && session.closed() && trace.copy()==std::vector<int>({70}),
          "external caller can finish an interrupted shutdown");
}

void concurrent_erase_and_close() {
    Trace trace; Runtime runtime({.workers=1,.capacity=4}); Session session(runtime); Gate gate;
    auto object=session.objects().emplace<Resource>(trace,80);
    check(bool(object),"concurrent resource accepted");
    std::atomic<bool> erase_ok=false, close_ok=false;
    JoiningThread reader,eraser,closer;
    ReleaseGate cleanup{gate};
    reader.start([&]{(void)object->with([&](Resource&){gate.wait_inside();});});
    gate.wait_entered();
    eraser.start([&]{erase_ok=bool(session.objects().erase(*object));});
    const auto deadline=std::chrono::steady_clock::now()+5s;
    while(!object->expired()) {
        check(std::chrono::steady_clock::now()<deadline,"erase must close admission");
        std::this_thread::yield();
    }
    session.request_close();
    closer.start([&]{close_ok=bool(session.close());});
    check(trace.copy().empty() && !erase_ok && !close_ok,"erase and session close both wait for the same active access");
    gate.release(); reader.join(); eraser.join(); closer.join();
    check(erase_ok && close_ok && trace.copy()==std::vector<int>({80}) && session.objects().size()==0,
          "concurrent erase and close destroy the object exactly once");
}

void drain_preserves_sessions() {
    Trace trace; Runtime runtime({.workers=1,.capacity=4}); Session session(runtime); Gate blocker;
    auto object=session.objects().emplace<Resource>(trace,50);
    check(bool(object),"drain resource accepted");
    check(bool(runtime.post([&]{blocker.wait_inside();})),"blocking work accepted"); blocker.wait_entered();
    std::atomic<bool> ran=false;
    check(bool(session.post(runtime,[&]{ran=object->with([](Resource& r){check(r.value==50,"drained object valid");})==SubmitError::none;})),"session job queued");
    runtime.request_stop();
    bool rejected=false;
    try { (void)runtime.create_session(); } catch (const std::logic_error&) { rejected=true; }
    check(rejected,"new sessions are rejected after shutdown starts");
    blocker.release();
    check(bool(runtime.join()) && ran && trace.copy()==std::vector<int>({50}),"drain executes work before releasing session objects");
}
}

int main() {
    if (!Thread::supported()) {
        std::cout << "session tests require runtime worker support\n";
        return 77;
    }
    try {
        independent_lifetimes(); session_outlives_runtime(); active_access_and_close();
        tasks_and_external_executor(); individual_removal_and_reentry(); drain_preserves_sessions();
        reentrant_runtime_and_failure_cleanup(); destructor_reentry(); foreign_worker_can_retry_shutdown();
        concurrent_erase_and_close(); stop_callback_releases_session();
        std::cout << "session and runtime object lifetimes passed\n";
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
