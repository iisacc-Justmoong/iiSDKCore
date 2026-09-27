#include "BrowserLoop.h"

#include <emscripten/html5.h>
#include <exception>
#include <stdexcept>

namespace ii::runtime {
struct BrowserLoop::Impl {
    EventLoop& loop;
    PumpBudget budget;
    std::optional<int> request;
    bool active = false;
    std::exception_ptr failure;

    void schedule() {
        request = emscripten_request_animation_frame(&frame, this);
        if (*request < 0) throw std::runtime_error("Browser animation frame registration failed");
    }
    static EM_BOOL frame(double, void* context) {
        auto& self = *static_cast<Impl*>(context);
        self.request.reset();
        try {
            self.loop.poll(self.budget);
            if (self.active && self.loop.state() != LoopState::stopped) {
                if (!self.request) self.schedule();
            } else self.active = false;
        } catch (...) {
            // Never unwind across the browser C callback boundary.
            self.failure = std::current_exception();
            self.active = false;
            self.loop.request_stop();
        }
        return false;
    }
};

BrowserLoop::BrowserLoop(EventLoop& loop, PumpBudget budget)
    : impl_(std::make_unique<Impl>(loop, budget)) {
    if (budget.max_callbacks == 0 || budget.max_time <= Clock::duration::zero())
        throw std::invalid_argument("BrowserLoop requires a positive pump budget");
}
BrowserLoop::~BrowserLoop() { stop(); }
void BrowserLoop::start() {
    if (impl_->active) return;
    rethrow_if_failed();
    if (impl_->loop.state() == LoopState::stopped)
        throw std::logic_error("Cannot start a stopped EventLoop");
    impl_->schedule();
    impl_->active = true;
}
void BrowserLoop::stop() {
    if (impl_->request) emscripten_cancel_animation_frame(*impl_->request);
    impl_->request.reset();
    impl_->active = false;
}
bool BrowserLoop::running() const noexcept { return impl_->active; }
void BrowserLoop::rethrow_if_failed() const {
    if (impl_->failure) std::rethrow_exception(impl_->failure);
}
} // namespace ii::runtime
