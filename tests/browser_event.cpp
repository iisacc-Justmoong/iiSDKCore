#include <iiSDKCore/Runtime/BrowserLoop.h>
#include <iiSDKCore/Runtime/FrameLoop.h>
#include <iiSDKCore/Runtime/Application.h>
#include <iiSDKCore/Runtime/HostLoop.h>
#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace ii::runtime;
using namespace std::chrono_literals;

EM_JS(void, install_animation_probe, (), {
    globalThis.iisdkAnimationRequests = 0;
    globalThis.requestAnimationFrame = function(callback) {
        ++globalThis.iisdkAnimationRequests;
        return setTimeout(function() { callback(performance.now()); }, 16);
    };
    globalThis.cancelAnimationFrame = function(id) { clearTimeout(id); };
});
EM_JS(int, animation_requests, (), { return globalThis.iisdkAnimationRequests; });

struct BrowserEventTest final {
    EventLoop events{8};
    BrowserLoop driver{events,{1,1s}};
    EventLoop frame_events{8};
    FrameLoop frames{frame_events};
    BrowserLoop frame_driver{frames};
    EventLoop failure_events{8},cancelled_events{8};
    BrowserLoop failure_driver{failure_events};
    EventLoop extreme_events{8};
    BrowserLoop extreme_driver{extreme_events,{1,1s}};
    int calls=0;
    int frame_calls=0;
    int suspended_frame_requests=0;
    bool cancelled_ran=false;
    int extreme_calls=0;
    std::atomic<bool> posted{false},post_failed{false};
    Clock::time_point deadline=Clock::now()+5s;

    static void fail(const char* message) {
        std::cerr<<message<<'\n';
        emscripten_force_exit(1);
    }
    static void start(void* context) {
        auto& self=*static_cast<BrowserEventTest*>(context);
        if(animation_requests()!=0 || self.events.stats().executed!=0 || self.events.next_deadline())
            fail("idle EventLoop requested an animation frame or dispatched work");
        auto post=[&self] {
          if(!self.events.post([&self] {
            ++self.calls;
            self.driver.stop(); self.driver.start();
            (void)self.events.post([&self] { ++self.calls; });
          })) self.post_failed=true;
          self.posted=true;
        };
#if defined(__EMSCRIPTEN_PTHREADS__)
        std::thread(std::move(post)).detach();
#else
        post();
#endif
        emscripten_set_timeout(&after_posts,40,context);
    }
    static void after_posts(void* context) {
        auto& self=*static_cast<BrowserEventTest*>(context);
        if((!self.posted || self.calls!=2) && Clock::now()<self.deadline) {
            emscripten_set_timeout(&after_posts,10,context); return;
        }
        if(!self.posted || self.post_failed || self.calls!=2 || animation_requests()!=0) fail("post or restart failed");
        if(!self.events.after(60ms,[&self] { ++self.calls; })) fail("schedule browser deadline");
        self.events.suspend();
        emscripten_set_timeout(&resume,30,context);
    }
    static void resume(void* context) {
        auto& self=*static_cast<BrowserEventTest*>(context);
        if(self.calls!=2) fail("suspended event loop ran a timer");
        self.events.resume();
        emscripten_set_timeout(&after_timer,80,context);
    }
    static void after_timer(void* context) {
        auto& self=*static_cast<BrowserEventTest*>(context);
        if(self.calls!=3 && Clock::now()<self.deadline) {
            emscripten_set_timeout(&after_timer,10,context); return;
        }
        if(self.calls!=3 || animation_requests()!=0) fail("deadline did not wake the browser event loop");
        self.driver.stop();
        if(!self.events.post([&self] { ++self.calls; })) fail("post while browser driver stopped");
        emscripten_set_timeout(&restart,30,context);
    }
    static void restart(void* context) {
        auto& self=*static_cast<BrowserEventTest*>(context);
        if(self.calls!=3) fail("stopped browser driver dispatched work");
        self.driver.start();
        emscripten_set_timeout(&finish,40,context);
    }
    static void finish(void* context) {
        auto& self=*static_cast<BrowserEventTest*>(context);
        if(self.calls!=4 && Clock::now()<self.deadline) {
            emscripten_set_timeout(&finish,10,context); return;
        }
        if(self.calls!=4 || animation_requests()!=0) fail("browser restart or idle animation policy failed");
        self.driver.stop();
        self.driver.rethrow_if_failed();
        if(!self.frames.add([&self](const FrameContext&) {
            if(++self.frame_calls==1) {
                self.frame_events.suspend();
                self.suspended_frame_requests=animation_requests();
                emscripten_set_timeout(&resume_frames,50,&self);
            } else if(self.frame_calls==3) self.frame_events.request_stop();
        })) fail("register frame callback");
        self.frame_driver.start();
        emscripten_set_timeout(&finish_frames,100,context);
    }
    static void resume_frames(void* context) {
        auto& self=*static_cast<BrowserEventTest*>(context);
        if(self.frame_calls!=1 || animation_requests()!=self.suspended_frame_requests)
            fail("suspended frame loop kept requesting display frames");
        self.frame_events.resume();
    }
    static void finish_frames(void* context) {
        auto& self=*static_cast<BrowserEventTest*>(context);
        if(self.frame_driver.running() && Clock::now()<self.deadline) {
            emscripten_set_timeout(&finish_frames,10,context); return;
        }
        if(self.frame_calls!=3 || animation_requests()!=3 || self.frame_driver.running())
            fail("frame loop did not retain animation-frame pacing");
        self.frame_driver.rethrow_if_failed();
        {
            BrowserLoop temporary{self.cancelled_events};
            temporary.start();
            (void)self.cancelled_events.post([&self] { self.cancelled_ran=true; });
        }
        (void)self.failure_events.post([] { throw std::runtime_error("expected browser callback failure"); });
        self.failure_driver.start();
        emscripten_set_timeout(&finish_failure,50,context);
    }
    static void finish_failure(void* context) {
        auto& self=*static_cast<BrowserEventTest*>(context);
        if(self.failure_driver.running() && Clock::now()<self.deadline) {
            emscripten_set_timeout(&finish_failure,10,context); return;
        }
        bool caught=false;
        try { self.failure_driver.rethrow_if_failed(); }
        catch(const std::runtime_error&) { caught=true; }
        if(!caught || self.failure_driver.running() || self.cancelled_ran ||
           self.failure_events.state()!=LoopState::stopped)
            fail("browser failure or destruction barrier failed");
        const auto first=self.extreme_events.on_signal([&self] { ++self.extreme_calls; });
        const auto second=self.extreme_events.on_signal([&self] { ++self.extreme_calls; });
        auto sender=self.extreme_events.sender();
        if(!first || !second || !sender.signal(first.token,Clock::time_point::min()) ||
           !sender.signal(second.token,Clock::time_point::min())) fail("schedule extreme deadlines");
        self.extreme_driver.start();
        emscripten_set_timeout(&finish_extreme,40,context);
    }
    static void finish_extreme(void* context) {
        auto& self=*static_cast<BrowserEventTest*>(context);
        if(self.extreme_calls!=2 && Clock::now()<self.deadline) {
            emscripten_set_timeout(&finish_extreme,10,context); return;
        }
        if(self.extreme_calls!=2) fail("extreme past deadline failed to dispatch");
        self.extreme_driver.stop();
        std::cout<<"browser event wake contracts passed\n";
        emscripten_force_exit(0);
    }
};

int main() {
    {
        Application app;
        if (app.workers().stats().workers || app.workers().allocated_slots() || app.has_frame_loop() ||
            app.post([]{}).error!=SubmitError::overloaded || !app.workers().wait_idle()) return 1;
        HostLoop host(app.event_loop(),[]() noexcept {});
        if (host.io_waiter_active()) return 2;
#if !defined(__EMSCRIPTEN_PTHREADS__)
        bool rejected=false;
        try { (void)app.event_loop().watch(ii::io::Channel{},ii::io::Interest::readable,[](auto){}); }
        catch (const std::runtime_error&) { rejected=true; }
        if (!rejected || app.event_loop().stats().pending || host.io_waiter_active()) return 3;
#endif
        if (!host.close()) return 4;
#if !defined(__EMSCRIPTEN_PTHREADS__)
        EventLoop pending(2);
        const auto watch=pending.watch(ii::io::Channel{},ii::io::Interest::readable,[](auto){});
        bool attachment_rejected=false;
        try { HostLoop unsupported(pending,[]() noexcept {}); }
        catch (const std::runtime_error&) { attachment_rejected=true; }
        if (!watch || !attachment_rejected || !watch.token.pending()) return 5;
        HostLoop manual(pending,[]() noexcept {},{.monitor_io=false});
        if (!watch.token.cancel() || !manual.close()) return 6;
#endif
    }
    install_animation_probe();
    static BrowserEventTest test;
    test.driver.start();
    emscripten_set_timeout(&BrowserEventTest::start,70,&test);
    emscripten_exit_with_live_runtime();
}
