#include <iiSDKCore/Runtime/HostLoop.h>
#include <iiSDKCore/Runtime/Operation.h>
#include <iiSDKCore/Runtime/FrameLoop.h>
#include <iiSDKCore/Process/Process.h>
#include <atomic>
#include <array>
#include <condition_variable>
#include <iostream>
#include <latch>
#include <stdexcept>

using namespace ii::runtime;
using namespace std::chrono_literals;
namespace {
int checks=0;
void check(bool value,const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
struct Signal final {
    std::mutex mutex; std::condition_variable changed; bool pending=false;
    void notify() noexcept { std::lock_guard lock(mutex); pending=true; changed.notify_all(); }
    bool wait(Clock::duration timeout) {
        std::unique_lock lock(mutex);
        const auto ready=changed.wait_for(lock,timeout,[&] { return pending; });
        pending=false; return ready;
    }
};
void contracts() {
    static_assert(TaskExecutor<EventLoop::Sender> && TimedTaskExecutor<EventLoop::Sender>);
    EventLoop::Sender retired;
    {
        EventLoop loop(4); retired=loop.sender();
        bool inspected=false;
        HostLoop host(loop,[&]() noexcept { (void)loop.stats(); inspected=true; },{false});
        check(inspected,"wake callback runs outside the EventLoop mutex");
        bool duplicate=false;
        try { HostLoop other(loop,[]() noexcept {},{false}); } catch(const std::logic_error&) { duplicate=true; }
        check(duplicate,"one external host per loop");
        for (int i=0;i<4;++i) check(bool(retired.post([]{})),"bounded producer submission");
        check(retired.post([]{}).error==SubmitError::full,"endpoint preserves bounded admission");
        check(host.stats().wake_signals==1,"pending native wake coalesces");
        check(loop.poll({4,1s})==4,"owner dispatches admitted work");
        const auto timer=retired.after(1h,[]{});
        check(bool(timer),"weak endpoint timer");
        loop.poll(); const auto signals=host.stats().wake_signals;
        check(timer.token.cancel() && host.stats().wake_signals==signals+1,"direct TaskToken cancellation wakes the host");
        bool foreign=false,blocking=false;
        std::thread worker([&] { try { loop.poll(); } catch(const std::logic_error&) { foreign=true; } });
        worker.join();
        try { loop.run(); } catch(const std::logic_error&) { blocking=true; }
        check(foreign && blocking,"host binding pins dispatch and prohibits a second blocking driver");
        auto scope=std::make_unique<TaskScope>(); bool late=false;
        const auto timed=scope->sender().at(retired,Clock::now()+1h,[&]{late=true;});
        check(bool(timed) && bool(scope->close()) && !timed.token.pending(),"scoped timed work cancels through a structural endpoint");
        check(loop.poll()==0 && !late,"removed receiver never receives its timer");
        check(bool(host.close()) && !host.stats().connected && bool(host.close()),"idempotent host detach");
    }
    check(retired.post([]{}).error==SubmitError::stopped && retired.after(1ms,[]{}).error==SubmitError::stopped,
          "producer endpoint never retains a removed loop");
    {
        auto owner=std::make_unique<EventLoop>();
        auto connection=std::make_unique<HostLoop>(*owner,[]() noexcept {},HostLoopOptions{false});
        auto weak=owner->sender(); owner.reset();
        check(!connection->stats().connected && bool(connection->close()) && weak.post([]{}).error==SubmitError::stopped,
              "loop removal safely detaches a surviving connection");
    }
    {
        EventLoop events; HostLoop* current=nullptr; bool busy=false,wrong=false,poll_rejected=false;
        HostLoop connection(events,[&]() noexcept {
            if (!current) return;
            auto closed=current->close(); busy=!closed && closed.error().code==ii::core::ErrorCode::busy;
            try { events.poll(); } catch(const std::logic_error&) { poll_rejected=true; }
        },{false});
        current=&connection; events.poll();
        check(bool(events.post([]{})) && busy && poll_rejected,"wake callbacks cannot detach or pump inline");
        std::thread foreign([&] { auto closed=connection.close(); wrong=!closed && closed.error().code==ii::core::ErrorCode::wrong_executor; });
        foreign.join(); check(wrong,"foreign detach is rejected");
        check(bool(connection.close()) && events.poll()==1,"detach preserves caller-owned queued work");
    }
    EventLoop loop;
    std::latch entered(1),release(1);
    bool armed=false;
    HostLoop host(loop,[&]() noexcept { if (armed) { entered.count_down(); release.wait(); } },{false});
    loop.poll(); armed=true;
    std::thread producer([&] { (void)loop.post([]{}); });
    entered.wait();
    std::atomic<bool> returned=false;
    std::thread unblock([&] { std::this_thread::sleep_for(20ms); check(!returned.load(),"close waits for in-flight wake"); release.count_down(); });
    check(bool(host.close()),"detach after producer race"); returned=true;
    producer.join(); unblock.join();
}
void completion_and_frames() {
    EventLoop loop;
    Signal signal;
    HostLoop host(loop,[&]() noexcept { signal.notify(); });
    Runtime workers({.workers=1}); OperationOwner operations; TaskScope view;
    FrameLoop frames(loop,{.capacity=4}); int phase=0; bool delivered=false;
    check(bool(frames.add([&](const FrameContext&){phase=1;})),"frame update registration");
    check(bool(frames.add([&](const FrameContext&){check(phase==1,"frame phase order");phase=2;},
        {.phase=FramePhase::prepare_present})),"frame prepare registration");
    std::latch release_work(1);
    auto work=operations.submit<int>(workers,[&](WorkContext&)->ii::core::Result<int>{
        release_work.wait(); return 42;
    });
    const auto observation=work.wait_for(1s); release_work.count_down();
    check(HostLoop::is_current_thread() && observation==OperationWait::would_block,
          "tracked work cannot block an external host thread");
    {
        auto endpoint=loop.sender();
        check(work.then_on(endpoint,view.sender(),[&](auto value) {
            check(value->value==42,"typed operation result"); delivered=true; frames.step();
        }),"completion owns a weak endpoint copy");
    }
    const auto deadline=Clock::now()+2s;
    while (!delivered && Clock::now()<deadline) {
        check(signal.wait(1s),"external wake delivers worker completion"); loop.poll();
    }
    check(delivered && phase==2,"worker completion and frame stages share host thread");
    check(bool(workers.join()) && bool(operations.close()) && bool(view.close()),"joined background work");
    loop.request_stop(); check(bool(host.close()) && !HostLoop::is_current_thread(),"stop/detach releases host execution context");
}
void readiness(const char* fixture) {
    if (!ii::process::capabilities().launch) return;
    auto child=ii::process::Process::start({.arguments={fixture,"delayed-output"}});
    check(bool(child),"launch delayed native pipe fixture");
    std::array<std::byte,1> ready{};
    check(bool(child->output().wait(ii::io::Interest::readable,10s)),"fixture startup readiness");
    auto startup=child->output().read(ready);
    check(startup && startup->bytes==1 && ready[0]==std::byte{'R'},"fixture startup handshake");
    EventLoop loop; Signal signal;
    HostLoop host(loop,[&]() noexcept { signal.notify(); });
    auto endpoint=loop.sender(); ii::io::Buffer buffer(2); bool delivered=false;
    const auto owner=std::this_thread::get_id(); const auto started=Clock::now();
    check(bool(endpoint.read(child->output(),{buffer,0,2,3s},[&](auto reply) {
        check(std::this_thread::get_id()==owner,"readiness callback stays on host owner");
        check(reply && reply->transfer.bytes==2 && buffer.bytes()[0]==std::byte{'4'},"native pipe data delivered");
        delivered=true;
    })),"admit host I/O");
    const std::array release{std::byte{'g'}};
    const auto sent=child->input().write(release);
    check(sent && *sent==1,"release ready fixture after I/O admission");
    while (!delivered && Clock::now()-started<2s) {
        check(signal.wait(1s),"I/O readiness wakes an otherwise idle external host"); loop.poll();
    }
    check(delivered && Clock::now()-started<1s && host.stats().io_waits>0,"readiness precedes long request timeout");
    const auto waits=host.stats().io_waits;
    std::this_thread::sleep_for(50ms);
    check(host.stats().io_waits<=waits+1,"no periodic I/O waits after pending I/O drains");
    check(bool(host.close()),"close readiness host");
    std::cout<<"HOST_IO_OK waits="<<waits<<" latency_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count()<<'\n';
}
}
int main(int argc,char** argv) {
    if (!Thread::supported()) { std::cout<<"HostLoop threaded-host tests unsupported on this single-thread target\n";return 77; }
    try {
        contracts(); completion_and_frames(); if (argc>1) readiness(argv[1]);
        std::cout<<"HostLoop: "<<checks<<" checks passed\n"; return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
