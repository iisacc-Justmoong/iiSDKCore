#include <iiSDKCore/Runtime/Application.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace ii;
namespace {
void check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
void good(core::Result<void> r) { if (!r) throw std::runtime_error(r.error().message); }
void lifecycle() {
    runtime::Application app({"example","--test"},{.workers={.workers=1}});
    static_assert(runtime::TaskExecutor<runtime::Application>);
    check(app.workers().stats().workers==1,"SDK access did not reuse the application worker runtime");
    { runtime::SerialQueue sdk_queue(app.workers()); check(bool(sdk_queue.close()),"Existing SDK execution domains cannot use application workers"); }
    std::string order;
    good(app.attach({"model",[&](auto&) -> core::Result<void> { order+='A'; return {}; },[&]() -> core::Result<void> { order+='a'; return {}; }}));
    good(app.attach({"view",[&](auto& running) -> core::Result<void> {
        check(&running.event_loop()==&app.event_loop(),"Application event loop changed"); order+='B'; return {};
    },[&]() -> core::Result<void> { order+='b'; return {}; }}));
    bool quit=false;
    good(app.set_driver({[&](const auto& start) -> core::Result<void> {
        good(start()); check(app.state()==runtime::ApplicationState::running,"Start did not activate app");
        check(app.arguments().size()==2 && app.arguments()[1]=="--test","Argument ownership lost");
        good(app.quit(7)); return {};
    },[&]() -> core::Result<void> { quit=true; return {}; }}));
    auto result=app.exec();
    check(result && *result==7 && quit && order=="ABba","Attachment order/exit status incorrect");
    check(app.state()==runtime::ApplicationState::stopped && app.stop_token().stop_requested(),"Stop barrier not observed");
    check(!app.exec() && !app.attach({}) && !app.quit(),"Stopped app accepted lifecycle work");
    check(app.post([]{}).error==runtime::SubmitError::stopped,"Stopped app accepted worker work");
}
void rollback() {
    runtime::Application app;
    std::string order;
    good(app.attach({"first",[&](auto&) -> core::Result<void> { order+='A'; return {}; },[&]() -> core::Result<void> { order+='a'; return {}; }}));
    good(app.attach({"failure",[&](auto&) -> core::Result<void> { order+='B'; return core::fail(core::ErrorCode::failed,"activation failed"); },
        [&]() -> core::Result<void> { order+='b'; return core::fail(core::ErrorCode::failed,"cleanup failed"); }}));
    good(app.attach({"never",[&](auto&) -> core::Result<void> { order+='X'; return {}; },[]() -> core::Result<void> { return {}; }}));
    good(app.set_driver({[](const auto& start) { return start(); },[]() -> core::Result<void> { return {}; }}));
    auto result=app.exec();
    check(!result && result.error().message=="activation failed" && order=="ABba","Failure did not roll back attempted attachments in reverse");
}
void contracts() {
    runtime::Application app;
    check(!app.exec() && app.state()==runtime::ApplicationState::ready,"Missing driver consumed single-use app");
    check(!app.set_driver({}) && !app.attach({}),"Empty contracts accepted");
    auto attachment=runtime::ApplicationAttachment{"unique",[](auto&) -> core::Result<void> { return {}; },[]() -> core::Result<void> { return {}; }};
    good(app.attach(attachment)); check(!app.attach(attachment),"Duplicate attachment accepted");
    bool wrong=false;
    std::thread worker([&] { auto r=app.attach(attachment); wrong=!r && r.error().code==core::ErrorCode::wrong_executor; }); worker.join();
    check(wrong,"Foreign lifecycle mutation accepted");
    good(app.set_driver({[](const auto&) -> core::Result<void> { return {}; },[]() -> core::Result<void> { return {}; }}));
    check(!app.exec(),"Driver returning without start accepted");
}
void worker_shutdown() {
    std::atomic<bool> entered=false,finished=false;
    runtime::Application app({}, {.workers={.workers=1}});
    check(bool(app.post([&] {
        entered=true;
        while (!app.stop_token().stop_requested()) std::this_thread::yield();
        finished=true;
    })),"Worker not accepted");
    while (!entered) std::this_thread::yield();
    good(app.attach({"owner",[](auto&) -> core::Result<void> { return {}; },[&]() -> core::Result<void> {
        check(finished,"Owner detached before active workers retired"); return {};
    }}));
    good(app.set_driver({[](const auto& start) { return start(); },[]() -> core::Result<void> { return {}; }}));
    check(bool(app.exec()) && finished,"Shutdown did not join owned work");
}
void driver_failure() {
    runtime::Application app;
    int detached=0;
    good(app.attach({"retire",[](auto&) -> core::Result<void> { return {}; },[&]() -> core::Result<void> { ++detached; return {}; }}));
    good(app.set_driver({[](const auto& start) -> core::Result<void> {
        good(start()); (void)start(); return {};
    },[]() -> core::Result<void> { throw std::runtime_error("close"); }}));
    auto result=app.exec();
    check(!result && result.error().code==core::ErrorCode::busy && detached==1,"Duplicate start escaped the Core lifecycle contract");
    runtime::Application throwing;
    good(throwing.set_driver({[](const auto&) -> core::Result<void> { throw std::runtime_error("driver"); },[]() -> core::Result<void> { return {}; }}));
    auto failure=throwing.exec();
    check(!failure && failure.error().cause && throwing.state()==runtime::ApplicationState::stopped,"Driver exception did not retire application");
}
void frames() {
    using namespace std::chrono_literals;
    runtime::Application app({}, {.workers={.workers=1},.frames={.fixed_step=10ms}});
    auto& frames=app.frame_loop();check(&frames.events()==&app.event_loop(),"Application frame loop uses another event loop");
    int fixed=0,updates=0,ui=0,retired=0;
    struct Capture { int* retired;~Capture(){++*retired;} };
    auto owner=std::make_shared<Capture>(Capture{&retired});retired=0;
    check(bool(frames.add([owner,&fixed](const auto&){++fixed;},{.phase=runtime::FramePhase::fixed_update})),"Fixed frame registration failed");
    owner.reset();
    check(bool(frames.add([&](const auto&){++updates;})),"Per-frame update failed");
    check(bool(frames.add([&](const auto&){++ui;},{.clock=runtime::TickClock::unscaled})),"Unscaled UI tick failed");
    good(app.attach({"view",[](auto&)->core::Result<void>{return {};},[&]()->core::Result<void>{
        check(retired==1 && frames.stats().active_ticks==0,"Frame captures retained past attachment teardown");return {};
    }}));
    good(app.set_driver({[&](const auto& start)->core::Result<void>{
        good(start());const auto now=runtime::Clock::now();
        (void)frames.step(now);(void)frames.step(now+25ms);
        frames.set_paused(true);(void)frames.step(now+35ms);
        check(fixed==2 && updates==2 && ui==3,"Shared app/game fixed, variable and paused UI clocks differ");
        return {};
    },[]()->core::Result<void>{return {};} }));
    check(bool(app.exec()) && retired==1,"App did not retire frame work");
    check(!frames.add([](const auto&){}),"Stopped application accepted frame work");
    runtime::EventLoop events;runtime::FrameLoop loop(events);int pending=0;
    check(bool(loop.add([&](const auto&){loop.close();})),"Closing tick registration failed");
    check(bool(loop.add([&](const auto&){++pending;})),"Pending tick registration failed");
    (void)loop.step();check(pending==0 && loop.stats().active_ticks==0,"Closing a frame did not revoke pending ticks");
    loop.close();
}
}
int main(int argc,char** argv) {
    try {
        std::string_view test=argc>1?argv[1]:"lifecycle";
        if(test=="lifecycle") lifecycle(); else if(test=="rollback") rollback();
        else if(test=="contracts") contracts(); else if(test=="worker_shutdown") worker_shutdown();
        else if(test=="driver_failure") driver_failure();
        else if(test=="frames") frames();
        else throw std::runtime_error("Unknown test");
        std::cout<<"CORE_APPLICATION_OK "<<test<<'\n';
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
