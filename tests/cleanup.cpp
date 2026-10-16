#include "file_fixture.h"
#include <iiSDKCore/Runtime/TaskScope.h>
#include <iiSDKCore/Runtime/Query.h>
#include <iiSDKCore/Scene/Scene.h>
#include <iiSDKCore/IO/AsyncIO.h>
#include <iiSDKCore/Core/Task/TaskGroup.h>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>

using namespace ii::runtime;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered=false, released=false;
    void enter() {
        std::unique_lock lock(mutex); entered=true; changed.notify_all();
        if (!changed.wait_for(lock,5s,[&]{return released;})) throw std::runtime_error("gate timeout");
    }
    void wait() {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock,5s,[&]{return entered;})) throw std::runtime_error("start timeout");
    }
    void release() { std::lock_guard lock(mutex); released=true; changed.notify_all(); }
};
void queued_port_cleanup() {
    EventLoop loop(1);
    auto handler=std::make_shared<int>(1), payload=std::make_shared<int>(2), completion=std::make_shared<int>(3);
    std::weak_ptr<int> h=handler,p=payload,c=completion;
    bool called=false;
    TaskToken old;
    {
        QueryPort<std::shared_ptr<int>> port([handler](const auto&){});
        auto submitted=port.post(loop,payload,[completion,&called](auto){called=true;});
        check(bool(submitted),"queued port admission"); old=submitted.token;
        handler.reset(); payload.reset(); completion.reset();
    }
    check(h.expired() && p.expired() && c.expired(),"port removal releases handler, payload and completion");
    check(loop.stats().pending==0 && !old.cancel(),"port removal returns executor quota and invalidates old work");
    QueryPort<int,int> next([](const int& n){return n+1;});
    check(bool(next.post(loop,41,[&](auto reply){called=reply && *reply==42;})),"next object reuses all capacity");
    check(loop.poll({4,1s})==1 && called,"only the replacement object executes");
    check(bool(next.close()) && bool(next.close()),"port close is idempotent");
    check(next.post(loop,0,[](auto){}).error==SubmitError::stopped,"closed port rejects admission");
    check(!next.send(0),"closed port cannot invoke the old handler");
}
void loop_stop_cleanup() {
    EventLoop loop(1);
    auto resource=std::make_shared<int>(7); std::weak_ptr<int> weak=resource;
    check(bool(loop.after(1h,[resource]{})),"scheduled resource accepted"); resource.reset();
    loop.request_stop();
    check(weak.expired() && loop.stats().pending==0,"stop discards scheduled resources immediately");
}
void scene_cleanup() {
    Runtime runtime({.workers=1,.capacity=8}); SerialQueue queue(runtime);
    ii::scene::Scene scene(queue,2);
    auto resource=std::make_shared<int>(5); std::weak_ptr<int> weak=resource;
    check(bool(queue.post([&]{
        const auto parent=scene.create({{.name="value",.value=resource}}).value();
        const auto child=scene.create({},parent).value(); resource.reset();
        bool removed_reply=false;
        auto& properties=scene.properties(parent).value().get();
        check(bool(properties.queries().post(PropertyRead{},[&removed_reply](auto){removed_reply=true;})),"scene query queued");
        check(bool(scene.erase(parent)) && weak.expired(),"subtree resources are gone before erase returns");
        check(runtime.stats().pending==1 && !removed_reply,"erased node leaves no queued callback or slot");
        const auto replacement=scene.create({}).value();
        check(replacement!=parent && replacement!=child && !scene.parent(parent),"clean slot reuse keeps stale ids invalid");
        const auto another=scene.create({}).value();
        check(bool(scene.erase(replacement)) && bool(scene.erase(another)),"full scene capacity reusable");
    })),"scene cleanup admission");
    check(bool(runtime.wait_idle()),"scene cleanup completes");
}
void queue_cleanup() {
    Runtime runtime({.workers=1,.capacity=4}); Gate blocker;
    check(bool(runtime.post([&]{blocker.enter();})),"unrelated blocker admitted"); blocker.wait();
    auto resource=std::make_shared<int>(1); std::weak_ptr<int> weak=resource;
    bool ran=false; TaskToken old;
    {
        SerialQueue object(runtime);
        auto work=object.post([resource,&ran]{ran=true;}); old=work.token;
        check(bool(work) && bool(object.post([]{})) && bool(object.post([]{})),"fill object quota"); resource.reset();
    }
    check(weak.expired() && runtime.stats().pending==1 && !old.cancel(),"queue removal frees only its own work");
    SerialQueue replacement(runtime);
    for(int i=0;i<3;++i) check(bool(replacement.post([]{})),"all released quota reusable");
    check(bool(replacement.close()) && !replacement.post([]{}),"closed queue cleans and rejects new work");
    blocker.release(); check(bool(runtime.wait_idle()) && !ran,"unrelated work continues");
}
void active_cleanup() {
    Runtime runtime({.workers=2,.capacity=4}); SerialQueue queue(runtime); Gate handler,completion;
    auto resource=std::make_shared<int>(7); std::weak_ptr<int> weak=resource;
    auto reply_resource=std::make_shared<int>(8); std::weak_ptr<int> reply_weak=reply_resource;
    auto port=std::make_unique<QueryPort<int>>(queue,[resource,&handler](const int&){handler.enter();});
    check(bool(port->post(0,[reply_resource,&completion](auto){completion.enter();})),"active query admitted");
    resource.reset(); reply_resource.reset(); handler.wait();
    std::atomic<bool> finished=false; Thread remover;
    check(bool(remover.start([&](ThreadContext&){check(bool(port->close()),"active port close"); finished=true;})),"remover started");
    const auto deadline=Clock::now()+5s;
    while(port->post(0,[](auto){}).error!=SubmitError::stopped)
        check(Clock::now()<deadline,"close shuts admission before waiting");
    check(!finished && !weak.expired(),"active handler keeps removal pending");
    handler.release(); completion.wait();
    check(!finished && !reply_weak.expired(),"completion and its resources are part of removal");
    completion.release();
    check(bool(remover.join()) && finished && weak.expired() && reply_weak.expired(),"close waits through active work and capture cleanup");
    port.reset();
    check(bool(runtime.wait_idle()) && runtime.stats().pending==0,"active cleanup returns runtime quota");
}
void cancellation_retires_before_reuse() {
    EventLoop loop(1); Gate retired;
    struct Resource { Gate& gate; ~Resource(){gate.enter();} };
    auto resource=std::shared_ptr<Resource>(new Resource{retired});
    auto work=loop.post([resource]{}); resource.reset(); Thread canceller;
    check(bool(canceller.start([&](ThreadContext&){check(work.token.cancel(),"token cancellation");})),"canceller started");
    retired.wait();
    check(loop.stats().pending==1 && loop.post([]{}).error==SubmitError::full,"quota remains occupied while old captures retire");
    retired.release(); check(bool(canceller.join()),"capture destructor completed");
    check(loop.stats().pending==0 && bool(loop.post([]{})),"quota opens after resource cleanup");
}
void self_cleanup_rejected() {
    EventLoop loop(1); TaskScope scope;
    check(bool(scope.post(loop,[&]{auto r=scope.close(); check(!r && r.error().code==ThreadErrorCode::self_join,"self close rejected");})),"scope self test admitted");
    loop.poll({4,1s}); check(bool(scope.close()),"external scope cleanup completes");
}
void group_cleanup() {
    Runtime runtime({.workers=1,.capacity=2}); Gate blocker;
    check(bool(runtime.post([&]{blocker.enter();})),"group blocker admitted"); blocker.wait();
    auto resource=std::make_shared<int>(1); std::weak_ptr<int> weak=resource;
    bool called=false;
    {
        ii::core::TaskGroup group(runtime);
        check(bool(group.post([resource,&called]{called=true;})),"owned group task admitted"); resource.reset();
    }
    check(weak.expired() && runtime.stats().pending==1,"group removal releases task and quota");
    blocker.release(); check(bool(runtime.wait_idle()) && !called,"removed group never executes");
}
void stop_capture_reentry() {
    EventLoop loop(2); Thread worker; Gate entered;
    check(bool(loop.post([&]{entered.enter();})),"stop gate admitted");
    bool destroyed=false;
    auto resource=std::shared_ptr<int>(new int(1),[&](int* value){
        delete value;
        check(worker.state()==ThreadState::stop_requested,"stop cleanup may inspect its thread");
        auto joined=worker.join();
        check(!joined && joined.error().code==ThreadErrorCode::self_join,"stop cleanup cannot wait for itself");
        destroyed=true;
    });
    check(bool(loop.after(1h,[resource]{})) && bool(worker.start(loop)),"owned stop resource admitted");
    resource.reset(); entered.wait();
    check(worker.request_stop() && destroyed,"thread stop releases queued captures outside its locks");
    entered.release(); check(bool(worker.join()) && loop.stats().pending==0,"stop leaves no reservation");
}
void concurrent_admission_cleanup() {
    EventLoop loop(1); TaskScope scope; Gate submitting;
    struct Delayed {
        EventLoop& loop; Gate& gate;
        Submission post(Task task,Priority priority) { gate.enter(); return loop.post(std::move(task),priority); }
    } executor{loop,submitting};
    auto resource=std::make_shared<int>(1); std::weak_ptr<int> weak=resource;
    Thread producer,remover;
    check(bool(producer.start([&,resource](ThreadContext&){check(bool(scope.post(executor,[resource]{})),"racing task accepted");})),"delayed producer starts");
    resource.reset(); submitting.wait();
    check(bool(remover.start([&](ThreadContext&){check(bool(scope.close()),"concurrent close completes");})),"concurrent remover starts");
    submitting.release();
    check(bool(producer.join()) && bool(remover.join()) && weak.expired() && loop.stats().pending==0,"close accounts for in-flight admission and capture retirement");
}
void repeated_reuse() {
    EventLoop loop(1);
    for(int i=0;i<1000;++i) {
        auto resource=std::make_shared<int>(i); std::weak_ptr<int> weak=resource;
        {
            QueryPort<std::shared_ptr<int>> port([resource](const auto&){});
            check(bool(port.post(loop,resource,[resource](auto){})),"repeated full-slot admission"); resource.reset();
        }
        check(weak.expired() && loop.stats().pending==0,"every removed object releases all owned references and quota");
    }
}
void scope_failures_and_tokens() {
    TaskToken stale;
    auto resource=std::make_shared<int>(1); std::weak_ptr<int> weak=resource;
    {
        EventLoop loop(1); TaskScope scope;
        check(bool(loop.post([]{})),"fill rejection quota");
        check(scope.post(loop,[resource]{}).error==SubmitError::full,"scope rejected submission");
        struct Throwing {
            Submission post(Task,Priority) { throw std::runtime_error("executor rejected"); }
        } executor;
        bool threw=false;
        try { (void)scope.post(executor,[resource]{}); }
        catch(const std::runtime_error&) { threw=true; }
        check(threw,"submit exception propagated");
        resource.reset();
        check(weak.expired() && bool(scope.close()),"failed admission leaves no owned work");
        loop.poll({4,1s});
        stale=loop.post([]{}).token;
        check(stale.pending(),"live token observes reserved slot");
    }
    check(!stale.pending() && !stale.cancel(),"token is safe after executor destruction");
    auto finalizer_resource=std::make_shared<int>(2); std::weak_ptr<int> finalizer_weak=finalizer_resource;
    int calls=0;
    TaskScope failed([finalizer_resource,&calls]{++calls; throw std::runtime_error("cleanup failed");});
    finalizer_resource.reset();
    const auto result=failed.close();
    check(!result && result.error().code==ThreadErrorCode::work_failed && finalizer_weak.expired(),"failing finalizer still releases captures");
    check(!failed.close() && calls==1,"finalizer executes exactly once");
}
void synchronous_cleanup() {
    TaskScope scope; Gate invoking; Thread caller,remover;
    auto resource=std::make_shared<int>(1); std::weak_ptr<int> weak=resource;
    check(bool(caller.start([&,resource](ThreadContext&){check(scope.run([resource,&invoking]{invoking.enter();})==SubmitError::none,"scope synchronous run");})),"synchronous caller starts");
    resource.reset(); invoking.wait(); std::atomic<bool> finished=false;
    check(bool(remover.start([&](ThreadContext&){check(bool(scope.close()),"synchronous close"); finished=true;})),"synchronous remover starts");
    const auto deadline=Clock::now()+5s;
    while(!scope.closed()) check(Clock::now()<deadline,"synchronous close admission");
    check(!finished && !weak.expired() && scope.run([]{})==SubmitError::stopped,"synchronous operation holds cleanup open");
    invoking.release();
    check(bool(caller.join()) && bool(remover.join()) && weak.expired() && finished,"synchronous cleanup completes");
}
void scene_retirement_and_self_removal() {
    Runtime runtime({.workers=1,.capacity=4}); SerialQueue queue(runtime); ii::scene::Scene scene(queue,1);
    check(bool(queue.post([&]{
        ii::graph::NodeId node;
        bool retired=false;
        auto resource=std::shared_ptr<int>(new int(1),[&](int* value){
            delete value;
            check(!scene.parent(node),"removed scene identity is invalid during capture retirement");
            auto created=scene.create({});
            check(!created && created.error().code==ii::core::ErrorCode::full,"scene quota opens after resource destruction");
            retired=true;
        });
        node=scene.create({{.name="owned",.value=resource},{.name="remove",.method=[&](const auto&)->DynamicValue{
            auto removed=scene.erase(node);
            check(!removed && removed.error().code==ii::core::ErrorCode::busy,"self removal is rejected before topology changes");
            check(bool(scene.parent(node)),"rejected removal preserves identity");
            return {};
        }}}).value();
        resource.reset();
        auto& properties=scene.properties(node).value().get();
        check(bool(properties.apply(PropertyInvoke{{"remove"},{}})),"self removal method completes");
        check(bool(scene.erase(node)) && retired,"scene cleanup retires owned resources");
        check(bool(scene.create({})),"scene quota fully reusable afterwards");
    })),"scene retirement test admitted");
    const auto result=runtime.wait_idle();
    if (!result && result.error().cause) std::rethrow_exception(result.error().cause);
    check(bool(result) && runtime.stats().failed==0,"scene retirement test completes");
}
void async_io_cleanup() {
    Runtime runtime({.workers=1,.capacity=2}); Gate blocker;
    check(bool(runtime.post([&]{blocker.enter();})),"I/O blocker admitted"); blocker.wait();
    const auto path=std::filesystem::current_path()/"cleanup-io.tmp";
    auto opened=ii::testing::open_file(path,{ii::io::Access::read_write,ii::io::Creation::truncate});
    check(bool(opened),"cleanup I/O file opened");
    auto alias=*opened;
    auto resource=std::make_shared<int>(1); std::weak_ptr<int> weak=resource;
    bool called=false;
    {
        ii::io::AsyncIO object(runtime,std::move(*opened));
        check(bool(object.queries().post(ii::io::Read{ii::io::Buffer(4096)},[resource,&called](auto){called=true;})),"owned I/O request admitted");
        resource.reset();
        check(bool(object.close()) && weak.expired() && runtime.stats().pending==1,"I/O close retires pending requests and quota");
        check(bool(alias.size()),"externally owned channel remains usable");
    }
    check(bool(alias.close()),"external channel owner closes explicitly");
    check(std::filesystem::remove(path),"cleanup I/O test file removed");
    blocker.release(); check(bool(runtime.wait_idle()) && !called,"removed I/O never runs");
}
void runtime_cleanup_reentry() {
    Runtime runtime({.workers=1,.capacity=2}); Gate blocker;
    check(bool(runtime.post([&]{blocker.enter();})),"reentry blocker admitted"); blocker.wait();
    bool destroyed=false;
    auto resource=std::shared_ptr<int>(new int(1),[&](int* value){
        delete value;
        auto idle=runtime.wait_idle(), joined=runtime.join();
        check(!idle && idle.error().code==ThreadErrorCode::self_join && !joined && joined.error().code==ThreadErrorCode::self_join,"capture destruction cannot wait for its own retiring slot");
        destroyed=true;
    });
    const auto work=runtime.post([resource]{}); resource.reset();
    check(work.token.cancel() && destroyed && runtime.stats().pending==1,"runtime cancellation completes outside locks");
    blocker.release(); check(bool(runtime.wait_idle()),"reentry runtime completes");
}
}
int main() {
    try {
        queued_port_cleanup(); loop_stop_cleanup(); self_cleanup_rejected(); repeated_reuse(); scope_failures_and_tokens();
        if (Thread::supported()) {
            queue_cleanup(); active_cleanup(); cancellation_retires_before_reuse(); scene_cleanup(); group_cleanup();
            stop_capture_reentry(); concurrent_admission_cleanup(); synchronous_cleanup();
            scene_retirement_and_self_removal(); async_io_cleanup(); runtime_cleanup_reentry();
        }
        std::cout<<"clean removal contracts passed on "<<platform_name()<<'\n';
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
