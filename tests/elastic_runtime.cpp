#include <iiSDKCore/Runtime/ComputeRouter.h>
#include <iiSDKCore/Runtime/ManagedMemory.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace ii::runtime;
using namespace std::chrono_literals;
namespace {
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
struct Gate final {
    std::mutex mutex;std::condition_variable cv;int entered=0;bool open=false;
    void run(){std::unique_lock lock(mutex);++entered;cv.notify_all();if(!cv.wait_for(lock,5s,[&]{return open;}))throw std::runtime_error("gate timed out");}
    bool wait(int n){std::unique_lock lock(mutex);return cv.wait_for(lock,5s,[&]{return entered>=n;});}
    void release(){std::lock_guard lock(mutex);open=true;cv.notify_all();}
};
template<class F> bool until(F predicate){const auto limit=Clock::now()+5s;while(!predicate()&&Clock::now()<limit)std::this_thread::yield();return predicate();}
void explicit_resources(){
    Runtime runtime({.capacity=8});
    check(runtime.stats().workers==0 && runtime.allocated_slots()==0,
          "default runtime reserves neither workers nor worker slots");
    int called=0;
    check(runtime.post([&]{++called;}).error==SubmitError::overloaded && called==0,
          "unallocated background work is rejected without inline execution");
    EventLoop events(2);
    check(bool(events.post([&]{++called;})) && events.poll()==1 && called==1,
          "base event loop remains independently usable");
    check(bool(runtime.resize_workers(1)) && runtime.stats().workers==1 && runtime.allocated_slots()==8,
          "an explicit request allocates worker and bounded queue storage");
    const auto first=runtime.post([&]{++called;});
    check(first && runtime.wait_idle() && called==2,"explicitly allocated worker executes work");
    check(runtime.resize_workers(0) && runtime.stats().workers==0 && runtime.allocated_slots()==0,
          "returning workers releases their task storage");
    check(runtime.post([]{}).error==SubmitError::overloaded,"released workers refuse work");
    check(bool(runtime.resize_workers(1)),"allocate workers again");
    Gate gate;
    const auto second=runtime.post([&]{gate.run();});
    const bool entered=gate.wait(1);
    const bool stale=!first.token.pending() && !first.token.cancel();
    gate.release();
    check(second && entered && stale && runtime.wait_idle(),"old tokens cannot alias reallocated slots");
}
struct OwnedResources final {
    explicit OwnedResources(MemoryPool shared):pool(std::move(shared)),worker({.workers=1,.capacity=2}) {
        auto allocated=pool.allocate(32);
        if(!allocated) throw std::runtime_error("object memory allocation failed");
        block=std::move(*allocated);
    }
    MemoryPool pool;
    Runtime worker;
    MemoryBlock block;
};
void object_resource_lifetime(){
    Runtime base;
    MemoryPool budget(64);
    auto session=base.create_session();
    auto owned=session->objects().emplace<OwnedResources>(budget);
    check(bool(owned) && budget.stats().used==32,"object explicitly allocates bounded memory and worker");
    int completed=0;
    check(owned->with([&](auto& object) {
        check(bool(object.worker.post([&]{++completed;})) && object.worker.wait_idle(),
              "object-owned worker runs its submitted task");
    })==SubmitError::none && completed==1,"object resource is accessible during its lifetime");
    check(session->close() && owned->expired() && budget.stats().used==0,
          "session destruction retires object resources and returns memory");
    check(base.stats().workers==0 && base.allocated_slots()==0,
          "object allocation does not start the base runtime worker pool");
}
void allocation_and_queue_close_race(){
    for(int iteration=0;iteration<8;++iteration){
        Runtime runtime({.capacity=8});
        SerialQueue queue(runtime);
        std::latch start(1);
        ThreadResult allocated,closed;
        std::thread allocator([&]{start.wait();allocated=runtime.resize_workers(1);});
        std::thread closer([&]{start.wait();closed=queue.close();});
        start.count_down();allocator.join();closer.join();
        check(allocated && closed && queue.post([]{}).error==SubmitError::stopped,
              "queue closure remains safe while worker storage is allocated");
        check(runtime.resize_workers(0) && runtime.allocated_slots()==0,
              "racing allocation can still return all worker storage");
    }
}
void grow_and_shrink(){
    Runtime runtime({.workers=1,.capacity=8,.queues=2});
    check(bool(runtime.resize_workers(3))&&runtime.stats().workers==3,"grow actual workers");
    Gate gate;std::atomic<int> completed=0;
    for(int i=0;i<3;++i)check(bool(runtime.post([&]{gate.run();++completed;})),"accept parallel task");
    const bool entered=gate.wait(3);
    std::atomic<bool> done=false;ThreadResult result;
    std::thread shrink([&]{result=runtime.resize_workers(1);done=true;});
    const bool capacity_updated=until([&]{return runtime.stats().workers==1;});const bool awaited=!done;
    gate.release();shrink.join();
    check(entered&&capacity_updated&&awaited&&result&&completed==3&&runtime.stats().workers==1,"shrink updates admission capacity and waits for active work without loss");
    check(bool(runtime.post([&]{++completed;}))&&runtime.wait_idle()&&completed==4,"remaining worker executes work");
    check(!runtime.resize_workers(9),"worker count may not exceed task capacity");
}
void release_and_reacquire(){
    Runtime runtime({.workers=1,.capacity=4});
    ComputeRouter router;const auto zone=router.add_zone({.name="elastic"}).value();
    (void)router.add_target(runtime,zone).value();
    Gate gate;check(bool(runtime.post([&]{gate.run();})),"active task accepted");check(gate.wait(1),"task started");
    std::atomic<bool> done=false;ThreadResult result;
    std::thread retire([&]{result=runtime.resize_workers(0);done=true;});
    const bool closed=until([&]{auto r=runtime.post([]{});if(r){(void)runtime.cancel(r.token);return false;}return r.error==SubmitError::overloaded;});
    const bool waited=!done;auto joined=runtime.join();
    const bool busy=!joined&&joined.error().code==ThreadErrorCode::busy;
    gate.release();retire.join();
    check(closed&&waited&&busy&&result&&runtime.stats().workers==0,"zero workers drains then releases physical workers");
    check(runtime.post([]{}).error==SubmitError::overloaded,"paused pool refuses new work");
    check(router.acquire(router.default_scope()).error()==SubmitError::overloaded,"router does not place work on zero workers");
    check(bool(runtime.resize_workers(2))&&runtime.stats().workers==2,"workers can be reacquired");
    auto lease=router.acquire(router.default_scope());check(bool(lease),"router observes resumed workers");lease->complete();
    std::atomic<int> done_count=0;check(bool(runtime.post([&]{++done_count;}))&&runtime.wait_idle()&&done_count==1,"reacquired workers run tasks");
    check(bool(router.close(ShutdownMode::drain)),"router releases borrowed runtime");
}
void reentrant_resize(){
    Runtime runtime({.workers=1,.capacity=4});std::atomic<bool> rejected=false;
    check(bool(runtime.post([&]{auto result=runtime.resize_workers(0);rejected=!result&&result.error().code==ThreadErrorCode::self_join;})),"self-resize test accepted");
    check(runtime.wait_idle()&&rejected,"worker cannot join itself");
}
}
int main(){try{explicit_resources();object_resource_lifetime();allocation_and_queue_close_race();grow_and_shrink();release_and_reacquire();reentrant_resize();std::cout<<"ELASTIC_RUNTIME_OK\n";}
catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
