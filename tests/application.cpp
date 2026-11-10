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
    check(app.workers().stats().workers==0 && app.workers().allocated_slots()==0 && !app.has_frame_loop(),
          "Default application allocated unrequested workers or frame state");
    bool called=false;
    check(app.post([&]{called=true;}).error==runtime::SubmitError::overloaded && !called,
          "Application admitted work without a worker allocation");
    check(bool(app.event_loop().post([&]{called=true;})) && app.event_loop().poll()==1 && called,
          "Base event loop cannot dispatch explicitly submitted work");
    (void)app.frame_loop();
    check(app.has_frame_loop(),"Frame state was not allocated on explicit access");
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
void configuration_transaction() {
    runtime::Application success({}, {.workers={.workers=1}});
    std::string committed;
    good(success.configure_atomically([&](auto& owner) -> core::Result<void> {
        if (auto result=owner.set_driver({[](const auto& start) { return start(); },
                                         []() -> core::Result<void> { return {}; }}); !result) return result;
        return owner.attach({"committed",[&](auto&) -> core::Result<void> { committed+='A'; return {}; },
                             [&]() -> core::Result<void> { committed+='a'; return {}; }});
    }));
    check(bool(success.exec()) && committed=="Aa", "Successful configuration did not commit");
    auto stopped=success.configure_atomically([](auto&) -> core::Result<void> { return {}; });
    check(!stopped && stopped.error().code==core::ErrorCode::closed, "Stopped application accepted configuration");

    runtime::Application app({}, {.workers={.workers=1}});
    std::string order;
    good(app.attach({"existing",[&](auto&) -> core::Result<void> { order+='E'; return {}; },
                     [&]() -> core::Result<void> { order+='e'; return {}; }}));
    auto resource=std::make_shared<int>(1);
    std::weak_ptr<int> released=resource;
    auto failed=app.configure_atomically([owned=std::move(resource)](auto& owner) -> core::Result<void> {
        good(owner.set_driver({[owned](const auto&) -> core::Result<void> { return {}; },
                               [owned]() -> core::Result<void> { return {}; }}));
        good(owner.attach({"tentative",[owned](auto&) -> core::Result<void> { return {}; },
                           [owned]() -> core::Result<void> { return {}; }}));
        return owner.attach({"existing",[](auto&) -> core::Result<void> { return {}; },
                             []() -> core::Result<void> { return {}; }});
    });
    check(!failed && failed.error().code==core::ErrorCode::already_exists && released.expired(),
          "Failed configuration retained registrations or replaced its error");
    good(app.set_driver({[](const auto& start) { return start(); },[]() -> core::Result<void> { return {}; }}));
    good(app.attach({"tentative",[&](auto&) -> core::Result<void> { order+='T'; return {}; },
                     [&]() -> core::Result<void> { order+='t'; return {}; }}));
    check(bool(app.exec()) && order=="ETte", "Rolled-back registrations damaged the existing application");

    runtime::Application throwing({}, {.workers={.workers=1}});
    good(throwing.set_driver({[](const auto& start) { return start(); },[]() -> core::Result<void> { return {}; }}));
    auto exception=throwing.configure_atomically([](auto& owner) -> core::Result<void> {
        good(owner.attach({"retry",[](auto&) -> core::Result<void> { return {}; },
                           []() -> core::Result<void> { return {}; }}));
        throw std::runtime_error("configuration exception");
    });
    check(!exception && exception.error().code==core::ErrorCode::failed && exception.error().cause,
          "Configuration exception was lost");
    try { std::rethrow_exception(exception.error().cause); }
    catch (const std::runtime_error& error) {
        check(std::string_view(error.what())=="configuration exception", "Configuration exception was replaced");
    }
    good(throwing.attach({"retry",[](auto&) -> core::Result<void> { return {}; },
                          []() -> core::Result<void> { return {}; }}));
    check(!throwing.set_driver({[](const auto&) -> core::Result<void> { return {}; },
                                []() -> core::Result<void> { return {}; }}) && bool(throwing.exec()),
          "Rollback removed a pre-existing driver");

    runtime::Application guarded({}, {.workers={.workers=1}});
    bool destructor_rejected=false;
    struct Reenter final {
        runtime::Application* owner;
        bool* rejected;
        ~Reenter() {
            auto result=owner->attach({"reentrant",[](auto&) -> core::Result<void> { return {}; },
                                       []() -> core::Result<void> { return {}; }});
            *rejected=!result && result.error().code==core::ErrorCode::busy;
        }
    };
    auto guarded_failure=guarded.configure_atomically([&](auto& owner) -> core::Result<void> {
        auto nested=owner.configure_atomically([](auto&) -> core::Result<void> { return {}; });
        check(!nested && nested.error().code==core::ErrorCode::busy,"Nested configuration accepted");
        auto run=owner.exec();
        check(!run && run.error().code==core::ErrorCode::busy,"Execution entered during configuration");
        auto guard=std::make_shared<Reenter>(&owner,&destructor_rejected);
        good(owner.attach({"guard",[guard](auto&) -> core::Result<void> { return {}; },
                           []() -> core::Result<void> { return {}; }}));
        return core::fail(core::ErrorCode::failed,"intentional rollback");
    });
    check(!guarded_failure && destructor_rejected, "Capture destruction reentered a configuration rollback");
    bool wrong_thread=false;
    std::thread worker([&] { auto result=guarded.configure_atomically([](auto&) -> core::Result<void> { return {}; });
        wrong_thread=!result && result.error().code==core::ErrorCode::wrong_executor; });
    worker.join();
    check(wrong_thread, "Foreign thread entered application configuration");
    check(!guarded.configure_atomically({}), "Empty configuration callback was accepted");
}
void before_driver() {
    runtime::Application app({}, {.workers={.workers=1}});
    std::string order;
    good(app.attach({"view1",[&](auto& owner) -> core::Result<void> {
        check(owner.state()==runtime::ApplicationState::running,"View activated before driver readiness"); order+='X'; return {};
    },[&]() -> core::Result<void> { order+='x'; return {}; }}));
    good(app.attach_before_driver({"app1",[&](auto& owner) -> core::Result<void> {
        check(owner.state()==runtime::ApplicationState::starting,"Early owner missed application startup"); order+='A'; return {};
    },[&]() -> core::Result<void> { order+='a'; return {}; }}));
    good(app.attach({"view2",[&](auto&) -> core::Result<void> { order+='Y'; return {}; },
                     [&]() -> core::Result<void> { order+='y'; return {}; }}));
    good(app.attach_before_driver({"app2",[&](auto&) -> core::Result<void> { order+='B'; return {}; },
                                   [&]() -> core::Result<void> { order+='b'; return {}; }}));
    good(app.set_driver({[&](const auto& start) -> core::Result<void> {
        order+='D'; if (auto result=start(); !result) return result; order+='R'; return {};
    },[]() -> core::Result<void> { return {}; }}));
    check(bool(app.exec()) && order=="ABDXYRyxba", "Application and view owners did not start and retire in phase order");

    runtime::Application missing({}, {.workers={.workers=1}});
    bool early=false;
    good(missing.attach_before_driver({"early",[&](auto&) -> core::Result<void> { early=true; return {}; },
                                       []() -> core::Result<void> { return {}; }}));
    check(!missing.exec() && !early && missing.state()==runtime::ApplicationState::ready,
          "Missing driver activated or consumed an early owner");
    good(missing.set_driver({[](const auto& start) { return start(); },[]() -> core::Result<void> { return {}; }}));
    check(bool(missing.exec()) && early, "Early owner could not activate after a missing driver was supplied");

    runtime::Application failing({}, {.workers={.workers=1}});
    std::string rollback;
    good(failing.attach_before_driver({"first",[&](auto&) -> core::Result<void> { rollback+='A'; return {}; },
                                       [&]() -> core::Result<void> { rollback+='a'; return {}; }}));
    good(failing.attach_before_driver({"failure",[&](auto&) -> core::Result<void> { rollback+='B'; return core::fail(core::ErrorCode::failed,"early failure"); },
                                       [&]() -> core::Result<void> { rollback+='b'; return core::fail(core::ErrorCode::failed,"cleanup failure"); }}));
    good(failing.attach_before_driver({"never",[&](auto&) -> core::Result<void> { rollback+='X'; return {}; },
                                       []() -> core::Result<void> { return {}; }}));
    good(failing.attach({"view",[&](auto&) -> core::Result<void> { rollback+='V'; return {}; },
                         []() -> core::Result<void> { return {}; }}));
    good(failing.set_driver({[&](const auto&) -> core::Result<void> { rollback+='D'; return {}; },
                             []() -> core::Result<void> { return {}; }}));
    auto failed=failing.exec();
    check(!failed && failed.error().message=="early failure" && rollback=="ABba" &&
          failing.state()==runtime::ApplicationState::stopped, "Early failure entered the driver or lost rollback ordering");

    runtime::Application names({}, {.workers={.workers=1}});
    auto item=[](std::string name) { return runtime::ApplicationAttachment{std::move(name),
        [](auto&) -> core::Result<void> { return {}; }, []() -> core::Result<void> { return {}; }}; };
    good(names.attach(item("normal")));
    good(names.attach_before_driver(item("early")));
    check(!names.attach_before_driver(item("normal")) && !names.attach(item("early")),
          "Early and driver-ready names do not share one namespace");
    auto transaction=names.configure_atomically([&](auto& owner) -> core::Result<void> {
        good(owner.attach_before_driver(item("temporary_early")));
        good(owner.attach(item("temporary_view")));
        return core::fail(core::ErrorCode::failed,"cancel registration");
    });
    check(!transaction, "Failed mixed-phase configuration unexpectedly succeeded");
    good(names.attach_before_driver(item("temporary_view")));
    good(names.attach(item("temporary_early")));
    bool wrong=false;
    std::thread worker([&] { auto result=names.attach_before_driver(item("foreign"));
        wrong=!result && result.error().code==core::ErrorCode::wrong_executor; });
    worker.join();
    check(wrong,"A foreign thread registered an early owner");
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
void driver_stopped() {
    runtime::Application app({}, {.workers={.workers=1}});
    std::string order;
    check(!app.driver_stopped(), "A driver completion was accepted before execution");
    good(app.attach_before_driver({"application",[&](auto& owner) -> core::Result<void> {
        check(!owner.driver_stopped(), "A driver completed before it entered"); order+='A'; return {};
    },[&]() -> core::Result<void> { order+='a'; return {}; }}));
    good(app.attach({"view",[&](auto&) -> core::Result<void> { order+='V'; return {}; },
                     [&]() -> core::Result<void> { order+='v'; good(app.driver_stopped()); return {}; }}));
    bool wrong_thread=false;
    good(app.set_driver({[&](const auto& start) -> core::Result<void> {
        good(start());
        std::thread worker([&] { auto result=app.driver_stopped();
            wrong_thread=!result && result.error().code==core::ErrorCode::wrong_executor; });
        worker.join();
        check(wrong_thread && app.state()==runtime::ApplicationState::running, "Foreign completion retired the application");
        good(app.driver_stopped());
        check(app.state()==runtime::ApplicationState::stopped && app.stop_token().stop_requested(),
              "Completion did not retire the application before the driver returned");
        check(order=="AVva", "Completion did not detach in reverse phase order");
        check(app.post([]{}).error==runtime::SubmitError::stopped, "Stopped application accepted worker work");
        good(app.driver_stopped()); order+='R'; return {};
    },[]() -> core::Result<void> { return {}; }}));
    check(bool(app.exec()) && order=="AVvaR", "Driver return repeated or delayed completion");
}
void driver_stopped_during_start() {
    runtime::Application app({}, {.workers={.workers=1}});
    std::string order;
    good(app.attach_before_driver({"application",[&](auto&) -> core::Result<void> { order+='A'; return {}; },
                                  [&]() -> core::Result<void> { order+='a'; return {}; }}));
    good(app.attach({"first",[&](auto& owner) -> core::Result<void> {
        order+='B'; good(owner.driver_stopped());
        check(owner.state()==runtime::ApplicationState::running && !owner.stop_token().stop_requested(),
              "Completion retired an attachment while its activation was on the stack");
        order+='C'; return {};
    },[&]() -> core::Result<void> { order+='b'; return {}; }}));
    good(app.attach({"later",[&](auto&) -> core::Result<void> { order+='X'; return {}; },
                     [&]() -> core::Result<void> { order+='x'; return {}; }}));
    good(app.set_driver({[&](const auto& start) -> core::Result<void> {
        good(start());
        check(app.state()==runtime::ApplicationState::stopped && order=="ABCba",
              "Deferred completion started a later attachment or missed teardown");
        order+='R'; return {};
    },[]() -> core::Result<void> { return {}; }}));
    check(bool(app.exec()) && order=="ABCbaR", "Deferred completion did not preserve driver ownership");
}
void driver_stopped_before_start() {
    runtime::Application app({}, {.workers={.workers=1}});
    int retired=0;
    good(app.attach_before_driver({"application",[](auto&) -> core::Result<void> { return {}; },
                                  [&]() -> core::Result<void> { ++retired; return {}; }}));
    good(app.set_driver({[&](const auto& start) -> core::Result<void> {
        good(app.driver_stopped());
        check(app.state()==runtime::ApplicationState::stopped && retired==1, "Early driver completion did not retire the app");
        auto late=start();
        check(!late && late.error().code==core::ErrorCode::closed, "A completed driver restarted the app");
        return {};
    },[]() -> core::Result<void> { return {}; }}));
    auto result=app.exec();
    check(!result && result.error().code==core::ErrorCode::closed && retired==1,
          "An early completion lost its start failure or retired twice");
}
void driver_stopped_failure() {
    std::atomic<bool> entered=false,finished=false;
    runtime::Application app({}, {.workers={.workers=1}});
    check(bool(app.post([&] { entered=true; while (!app.stop_token().stop_requested()) std::this_thread::yield(); finished=true; })),
          "Owned worker was not accepted");
    while (!entered) std::this_thread::yield();
    int detached=0;
    good(app.attach_before_driver({"application",[](auto&) -> core::Result<void> { return {}; },
        [&]() -> core::Result<void> { ++detached; return core::fail(core::ErrorCode::failed,"later cleanup failure"); }}));
    good(app.attach({"view",[](auto&) -> core::Result<void> { return {}; },[&]() -> core::Result<void> {
        check(finished, "Completion detached the view before joining owned work"); ++detached;
        return core::fail(core::ErrorCode::failed,"first cleanup failure");
    }}));
    good(app.set_driver({[&](const auto& start) -> core::Result<void> {
        good(start()); good(app.driver_stopped());
        check(finished && detached==2 && app.state()==runtime::ApplicationState::stopped,
              "Completion did not finish cleanup before returning");
        return core::fail(core::ErrorCode::failed,"later driver failure");
    },[]() -> core::Result<void> { return {}; }}));
    const auto result=app.exec();
    check(!result && result.error().message=="first cleanup failure" && detached==2,
          "Completion replaced its first failure or repeated cleanup");
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
        else if(test=="contracts") contracts(); else if(test=="transaction") configuration_transaction();
        else if(test=="before_driver") before_driver();
        else if(test=="worker_shutdown") worker_shutdown();
        else if(test=="driver_failure") driver_failure();
        else if(test=="driver_stopped") driver_stopped();
        else if(test=="driver_stopped_during_start") driver_stopped_during_start();
        else if(test=="driver_stopped_before_start") driver_stopped_before_start();
        else if(test=="driver_stopped_failure") driver_stopped_failure();
        else if(test=="frames") frames();
        else throw std::runtime_error("Unknown test");
        std::cout<<"CORE_APPLICATION_OK "<<test<<'\n';
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
