#include "BrowserLoop.h"
#include "FrameLoop.h"
#include "HostLoop.h"

#include <emscripten/eventloop.h>
#include <emscripten/html5.h>
#if defined(__EMSCRIPTEN_PTHREADS__)
#include <emscripten/proxying.h>
#include <pthread.h>
#endif
#include <algorithm>
#include <atomic>
#include <exception>
#include <new>
#include <stdexcept>

namespace ii::runtime {
struct BrowserLoop::Impl {
    struct Bridge final {
        std::atomic<Impl*> owner{nullptr};
        std::atomic<bool> failed{false};
#if defined(__EMSCRIPTEN_PTHREADS__)
        pthread_t thread{};
#endif
    };
    struct Ticket final { std::shared_ptr<Bridge> bridge; };

    EventLoop& loop;
    PumpBudget budget;
    FrameLoop* frames = nullptr;
    std::optional<int> request;
    std::optional<int> timer;
    std::unique_ptr<Ticket> timer_ticket;
    std::unique_ptr<HostLoop> host;
    std::shared_ptr<Bridge> bridge=std::make_shared<Bridge>();
    bool active = false, pumping = false;
    std::exception_ptr failure;

    Impl(EventLoop& events,PumpBudget value):loop(events),budget(value) { bridge->owner=this; }
    ~Impl() { bridge->owner.store(nullptr,std::memory_order_release); }

    void schedule_frame() {
        request = emscripten_request_animation_frame(&frame, this);
        if (*request < 0) throw std::runtime_error("Browser animation frame registration failed");
    }
    static EM_BOOL frame(double, void* context) {
        auto& self = *static_cast<Impl*>(context);
        self.request.reset();
        try {
            self.pumping = true;
            if (self.active && self.loop.state() == LoopState::ready)
                self.frames->poll(self.budget);
            self.pumping = false;
            const auto state = self.loop.state();
            if (self.active && state == LoopState::ready) {
                if (!self.request) self.schedule_frame();
            } else if (!self.active || state == LoopState::stopped) {
                self.active = false;
                self.host.reset();
            }
        } catch (...) {
            self.pumping = false;
            self.failure = std::current_exception();
            self.active = false;
            self.loop.request_stop();
            self.host.reset();
        }
        return false;
    }
    static void deliver(void* context) {
        std::unique_ptr<Ticket> ticket(static_cast<Ticket*>(context));
        if (auto* owner=ticket->bridge->owner.load(std::memory_order_acquire)) owner->drive();
    }
#if defined(__EMSCRIPTEN_PTHREADS__)
    static void proxy(void* context) {
        // System-queue work may run inside a runtime call. Only post a later
        // browser event here; never dispatch an EventLoop callback inline.
        (void)emscripten_set_immediate(&deliver,context);
    }
#endif
    static void wake(const std::shared_ptr<Bridge>& bridge) noexcept {
        auto* ticket=new(std::nothrow) Ticket{bridge};
        if (!ticket) { bridge->failed=true; return; }
#if defined(__EMSCRIPTEN_PTHREADS__)
        if (!pthread_equal(pthread_self(),bridge->thread)) {
            if (!emscripten_proxy_async(emscripten_proxy_get_system_queue(),bridge->thread,&proxy,ticket)) {
                delete ticket; bridge->failed=true;
            }
            return;
        }
#endif
        (void)emscripten_set_immediate(&deliver,ticket);
    }
    void cancel_timer() {
        if (timer) emscripten_clear_timeout(*timer);
        timer.reset(); timer_ticket.reset();
    }
    static void timed(void* context) {
        auto* ticket=static_cast<Ticket*>(context);
        const auto bridge=ticket->bridge;
        if (auto* owner=bridge->owner.load(std::memory_order_acquire)) {
            (void)owner->timer_ticket.release();
            owner->timer.reset();
            std::unique_ptr<Ticket> released(ticket);
            owner->drive();
        }
    }
    void arm_timer() {
        const auto next=loop.next_deadline();
        if (!next) return;
        const auto now=Clock::now();
        const auto delay=*next<=now ? 0.0 :
            std::chrono::duration<double,std::milli>(core::Time::elapsed(now,*next)).count();
        timer_ticket=std::make_unique<Ticket>(Ticket{bridge});
        timer=emscripten_set_timeout(&timed,std::clamp(delay,0.0,2147483647.0),timer_ticket.get());
    }
    void drive() {
        if (!active || pumping) return;
        cancel_timer();
        try {
            if (bridge->failed.exchange(false)) throw std::runtime_error("Browser event wake could not be scheduled");
            pumping=true;
            // Frame work stays display-paced. A suspended pump only
            // acknowledges the host wake; it does not dispatch callbacks.
            if (!frames) loop.poll(budget);
            else if (loop.state()==LoopState::suspended)
                loop.poll({0,Clock::duration::zero()});
            pumping=false;
            const auto state=loop.state();
            if (active && state!=LoopState::stopped) {
                if (frames) {
                    if (state==LoopState::suspended && request) {
                        emscripten_cancel_animation_frame(*request);
                        request.reset();
                    } else if (state==LoopState::ready && !request) schedule_frame();
                } else arm_timer();
            }
            else { active=false; host.reset(); }
        } catch (...) {
            pumping=false;
            failure=std::current_exception();
            active=false;
            loop.request_stop();
            host.reset();
        }
    }
};

BrowserLoop::BrowserLoop(EventLoop& loop, PumpBudget budget)
    : impl_(std::make_unique<Impl>(loop,budget)) {
    if (budget.max_callbacks == 0 || budget.max_time <= Clock::duration::zero())
        throw std::invalid_argument("BrowserLoop requires a positive pump budget");
}
BrowserLoop::BrowserLoop(FrameLoop& frames, PumpBudget budget)
    : BrowserLoop(frames.events(), budget) {
    impl_->frames = &frames;
}
BrowserLoop::~BrowserLoop() { stop(); }
void BrowserLoop::start() {
    if (impl_->active) return;
    rethrow_if_failed();
    if (impl_->loop.state() == LoopState::stopped)
        throw std::logic_error("Cannot start a stopped EventLoop");
    impl_->active=true;
    try {
        if (!impl_->host) {
#if defined(__EMSCRIPTEN_PTHREADS__)
            impl_->bridge->thread=pthread_self();
#endif
            impl_->host=std::make_unique<HostLoop>(impl_->loop,
                [bridge=impl_->bridge]() noexcept { Impl::wake(bridge); },HostLoopOptions{false});
        }
        if (impl_->frames && impl_->loop.state()==LoopState::ready && !impl_->request)
            impl_->schedule_frame();
    } catch (...) { impl_->active=false; throw; }
}
void BrowserLoop::stop() {
    impl_->active=false;
    if (impl_->request) emscripten_cancel_animation_frame(*impl_->request);
    impl_->request.reset();
    impl_->cancel_timer();
    if (!impl_->pumping) impl_->host.reset();
}
bool BrowserLoop::running() const noexcept { return impl_->active; }
void BrowserLoop::rethrow_if_failed() const {
    if (impl_->failure) std::rethrow_exception(impl_->failure);
    if (impl_->bridge->failed.load(std::memory_order_acquire))
        throw std::runtime_error("Browser event wake could not be scheduled");
}
} // namespace ii::runtime
