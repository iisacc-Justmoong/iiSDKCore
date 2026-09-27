#pragma once

#include "EventLoop.h"

#if defined(__EMSCRIPTEN__)
namespace ii::runtime {

// Browser main-thread instance. EventLoop must outlive this driver.
// requestAnimationFrame owns pacing; each callback returns control to the host.
class BrowserLoop final {
public:
    explicit BrowserLoop(EventLoop& loop, PumpBudget budget = {});
    ~BrowserLoop();
    BrowserLoop(const BrowserLoop&) = delete;
    BrowserLoop& operator=(const BrowserLoop&) = delete;
    void start();
    void stop();
    [[nodiscard]] bool running() const noexcept;
    void rethrow_if_failed() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ii::runtime
#endif
