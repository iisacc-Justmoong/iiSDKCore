#include <iiSDKCore/Runtime/ChunkQueue.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace ii::runtime;
using namespace std::chrono_literals;
namespace {
void check(bool v,const char* m) { if(!v) throw std::runtime_error(m); }
template<class T> T take(ii::core::Result<T> value) { if(!value) throw std::runtime_error(value.error().message); return std::move(*value); }
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered=false, open=false;
    void wait() { std::unique_lock lock(mutex); entered=true; cv.notify_all(); cv.wait(lock,[&]{return open;}); }
    void await() { std::unique_lock lock(mutex); check(cv.wait_for(lock,3s,[&]{return entered;}),"external completion was not reached"); }
    void release() { {std::lock_guard lock(mutex);open=true;} cv.notify_all(); }
};
ChunkSnapshot phase(ChunkTicket& task,ChunkPhase wanted) {
    auto state=task.snapshot();
    const auto end=std::chrono::steady_clock::now()+3s;
    while(state.phase!=wanted && std::chrono::steady_clock::now()<end)
        state=task.wait_for_change(state.revision,100ms);
    check(state.phase==wanted,"task phase was not reached"); return state;
}
void producers() {
    ChunkQueue queue({.cpu_workers=3,.capacity=32});
    std::atomic<int> count=0;
    std::mutex mutex;
    std::vector<ChunkTicket> tasks;
    std::vector<std::thread> producers;
    for(int p=0;p<4;++p) producers.emplace_back([&] {
        for(int i=0;i<6;++i) {
            ChunkWork w; w.items=23; w.chunk_items=4;
            w.cpu=[&](ChunkRange r,std::stop_token) -> ii::core::Result<void> { count+=r.count; return {}; };
            auto task=take(queue.submit(std::move(w)));
            std::lock_guard lock(mutex);tasks.push_back(std::move(task));
        }
    });
    for(auto& t:producers)t.join();
    for(auto& task:tasks)check(phase(task,ChunkPhase::completed).completed_items==23,"CPU chunks lost work");
    check(count==4*6*23,"multiple producers or CPU chunks were lost");
    check(bool(queue.close()),"CPU queue did not close");
}
void gpu_control() {
    ChunkQueue queue({.cpu_workers=0,.capacity=4,.devices={{.group_size=4,.max_groups=2}}});
    auto gate=std::make_shared<Gate>();
    std::mutex geometry_mutex;
    std::vector<ChunkDispatch> geometry;
    std::atomic<int> collects=0,signals=0;
    ChunkWork w; w.items=14;w.chunk_items=10;
    w.device=[&](std::size_t index,ChunkDispatch d,std::stop_token) -> ii::core::Result<ChunkSubmission> {
        check(index==0,"wrong device lane");
        {std::lock_guard lock(geometry_mutex);geometry.push_back(d);}
        return ChunkSubmission{.wait=[gate]()->ii::core::Result<void>{gate->wait();return {};},
            .collect=[&]()->ii::core::Result<void>{++collects;return {};}};
    };
    w.interrupt=[&](std::uint64_t type,std::span<const std::byte> data)->ii::core::Result<void> {
        check(type==123 && data.size()==1,"custom interrupt changed");++signals;return {};
    };
    auto task=take(queue.submit(std::move(w)));gate->await();
    auto live=task.snapshot();
    check(live.device_submitted==1 && live.device_active==1 && live.completed_items==0,"submitted work was reported complete early");
    const std::byte data[]{std::byte{42}};
    check(bool(task.interrupt(123,data)) && signals==1,"custom interrupt was not delivered");
    check(bool(task.pause()),"pause rejected");
    check(task.snapshot().phase==ChunkPhase::pausing,"active device work did not enter pausing");
    gate->release();phase(task,ChunkPhase::paused);
    check(bool(task.resume()),"resume rejected");
    auto complete=phase(task,ChunkPhase::completed);
    check(complete.completed_items==14 && complete.device_submitted==2 && collects==2,"device work completion incorrect");
    check(geometry.size()==2 && geometry[0].range.offset==0 && geometry[0].range.count==8 &&
          geometry[0].groups==2 && geometry[1].range.offset==8 && geometry[1].range.count==6 &&
          geometry[1].groups==2,"device geometry ignored logical limits");
    check(!task.interrupt(123,data),"terminal task accepted an interrupt");
    check(bool(queue.close()),"device queue did not close");
}
void cancel_submitted() {
    ChunkQueue queue({.cpu_workers=0,.capacity=1,.devices={{.group_size=4,.max_groups=2}}});
    auto gate=std::make_shared<Gate>(); std::atomic<int> collects=0;
    ChunkWork work;work.items=32;work.chunk_items=8;
    work.device=[&](std::size_t,ChunkDispatch,std::stop_token)->ii::core::Result<ChunkSubmission>{
        return ChunkSubmission{.wait=[gate]()->ii::core::Result<void>{gate->wait();return {};},
                               .collect=[&]()->ii::core::Result<void>{++collects;return {};}};
    };
    auto task=take(queue.submit(work));gate->await();
    auto rejected=queue.submit(work);check(!rejected && rejected.error().code==ii::core::ErrorCode::full,"queue capacity ignored");
    check(bool(task.cancel()) && task.snapshot().phase==ChunkPhase::cancelling,"cancellation was reported complete early");
    gate->release();auto state=phase(task,ChunkPhase::cancelled);
    check(state.device_submitted==1 && state.completed_items==0 && collects==0,"cancelled device work was reclaimed or collected early");
    check(bool(queue.close()),"cancelled queue did not close");
}
void collection_and_close() {
    ChunkQueue queue({.cpu_workers=0,.capacity=1,.devices={{.group_size=4,.max_groups=2}}});
    auto gate=std::make_shared<Gate>();
    ChunkWork work;work.items=8;work.chunk_items=8;
    work.device=[gate](std::size_t,ChunkDispatch,std::stop_token)->ii::core::Result<ChunkSubmission> {
        return ChunkSubmission{.wait=[]()->ii::core::Result<void>{return {};},
                               .collect=[gate]()->ii::core::Result<void>{gate->wait();return {};}};
    };
    auto task=take(queue.submit(std::move(work)));gate->await();
    auto state=task.snapshot();
    if(state.device_active!=1 || state.device_in_flight!=0 || state.completed_items!=0) {
        gate->release();throw std::runtime_error("GPU completion and host collection were conflated");
    }
    std::atomic<bool> closed=false;
    std::thread closer([&]{closed=bool(queue.close());});
    try {
        phase(task,ChunkPhase::cancelling);
        check(!closed,"queue closed while host collection was active");
    } catch(...) {gate->release();closer.join();throw;}
    gate->release();closer.join();
    check(closed && task.snapshot().phase==ChunkPhase::cancelled,"queue did not retire active collection");
}
void cpu_cooperative_cancel() {
    ChunkQueue queue({.cpu_workers=1,.capacity=1});
    std::mutex mutex;std::condition_variable changed;bool entered=false;
    std::atomic<bool> observed=false;
    ChunkWork work;work.items=12;work.chunk_items=2;
    work.cpu=[&](ChunkRange,std::stop_token stop)->ii::core::Result<void> {
        std::unique_lock lock(mutex);
        std::stop_callback notify(stop,[&]{changed.notify_all();});
        entered=true;changed.notify_all();
        if(!changed.wait_for(lock,3s,[&]{return stop.stop_requested();}))
            return ii::core::fail(ii::core::ErrorCode::failed,"CPU stop was not delivered");
        observed=true;return ii::core::fail(ii::core::ErrorCode::cancelled,"cooperative stop");
    };
    auto task=take(queue.submit(std::move(work)));
    {std::unique_lock lock(mutex);check(changed.wait_for(lock,3s,[&]{return entered;}),"CPU callback did not start");}
    check(bool(task.cancel()),"CPU cancellation rejected");
    const auto result=phase(task,ChunkPhase::cancelled);
    check(observed && result.claimed_items==2 && result.completed_items==0,"CPU stop or unscheduled-work cancellation failed");
    check(bool(queue.close()),"CPU cancellation did not close");
}
}
int main() { try {producers();gpu_control();cancel_submitted();collection_and_close();cpu_cooperative_cancel();std::cout<<"chunk queue tests passed\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;} }
