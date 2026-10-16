#include <iiSDKCore/Runtime/BrowserLoop.h>
#include <iiSDKCore/Runtime/FrameLoop.h>
#include <iiSDKCore/Runtime/Runtime.h>
#include <emscripten.h>
#include <iostream>
#include <stdexcept>

using namespace ii::runtime;
using namespace std::chrono_literals;

struct BrowserContracts final {
    EventLoop successful{8}, failed{8}, cancelled{8};
    BrowserLoop success_driver{successful, {1, 1s}}, failure_driver{failed};
    EventLoop frame_events{8}, bad_frame_events{8};
    FrameLoop frames{frame_events}, bad_frames{bad_frame_events};
    BrowserLoop frame_driver{frames}, bad_frame_driver{bad_frames};
    int ui_frames = 0, simulation_frames = 0;
    int calls = 0;
    bool cancelled_ran = false;
    Clock::time_point deadline = Clock::now() + 5s;

    void start() {
        if (!frames.add([this](const FrameContext&) { ++simulation_frames; }) ||
            !frames.add([this](const FrameContext&) { ++ui_frames; },
                {.clock=TickClock::unscaled}) ||
            !frames.add([this](const FrameContext& time) {
                if (time.frame==1) frames.set_paused(true);
                if (time.frame==2) frames.set_paused(false);
                if (time.frame==3) frame_events.request_stop();
            },{.phase=FramePhase::end,.clock=TickClock::unscaled}) ||
            !bad_frames.add([](const FrameContext&) {
                throw std::runtime_error("expected browser frame failure");
            })) throw std::runtime_error("Cannot register browser frame tests");
        (void)successful.post([this] {
            ++calls;
            success_driver.stop();
            success_driver.start();
            (void)successful.post([this] { ++calls; successful.request_stop(); });
        });
        (void)failed.post([] { throw std::runtime_error("expected browser callback error"); });
        (void)cancelled.post([this] { cancelled_ran = true; });
        {
            BrowserLoop temporary{cancelled};
            temporary.start();
        }
        success_driver.start();
        success_driver.start(); // Idempotent; must not register a second frame chain.
        failure_driver.start();
        frame_driver.start();
        bad_frame_driver.start();
        emscripten_async_call(&verify, this, 50);
    }

    static void verify(void* context) {
        auto& self = *static_cast<BrowserContracts*>(context);
        if ((self.success_driver.running() || self.failure_driver.running() ||
             self.frame_driver.running() || self.bad_frame_driver.running()) &&
            Clock::now() < self.deadline) {
            emscripten_async_call(&verify, context, 50);
            return;
        }
        bool caught = false;
        try { self.failure_driver.rethrow_if_failed(); }
        catch (const std::runtime_error&) { caught = true; }
        bool frame_caught = false;
        try { self.bad_frame_driver.rethrow_if_failed(); }
        catch (const std::runtime_error&) { frame_caught = true; }
        const bool passed = self.calls == 2 && !self.cancelled_ran && caught &&
            !self.success_driver.running() && !self.failure_driver.running() &&
            self.failed.state() == LoopState::stopped && frame_caught &&
            self.bad_frame_events.state() == LoopState::stopped &&
            self.frames.stats().frames==3 && self.ui_frames==3 && self.simulation_frames==2 &&
            !self.frame_driver.running() && !self.bad_frame_driver.running();
        std::cout << (passed ? "browser contracts passed" : "browser contracts FAILED") << '\n';
        if (!passed) emscripten_force_exit(1);
    }
};

int main() {
    Thread worker;
    auto started = worker.start([](ThreadContext&) {});
    const auto expected = Thread::supported() ? ThreadErrorCode::would_block : ThreadErrorCode::unsupported;
    if (started || started.error().code != expected) return 1;
    EventLoop blocked;
    bool rejected = false;
    try { blocked.run(); } catch (const std::logic_error&) { rejected = true; }
    if (!rejected) return 2;
    bool runtime_rejected = false;
    try { Runtime runtime; } catch (const std::runtime_error&) { runtime_rejected = true; }
    if (!runtime_rejected) return 3;
    std::cout << "browser host blocking correctly rejected\n";
    static BrowserContracts contracts;
    contracts.start();
}
