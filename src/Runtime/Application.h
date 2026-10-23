#pragma once
#include "Runtime.h"
#include "TaskScope.h"
#include "FrameLoop.h"
#include "../Core/Result/Result.h"
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace ii::runtime {
class Application;
using ApplicationStart = std::function<core::Result<void>()>;
// Higher modules supply the OS driver. Core has no GUI or language dependency.
// run invokes start exactly once when attachment is safe, and returns after the
// native host has stopped. It must not retain start beyond the call.
struct ApplicationDriver final {
    std::function<core::Result<void>(const ApplicationStart& start)> run;
    std::function<core::Result<void>()> request_close;
};
struct ApplicationAttachment final {
    std::string name;
    std::function<core::Result<void>(Application&)> activate;
    std::function<core::Result<void>()> deactivate;
};
struct ApplicationOptions final {
    RuntimeOptions workers;
    std::size_t event_capacity = 1024;
    FrameLoopOptions frames;
};
enum class ApplicationState { ready, starting, running, stopping, stopped };
class Application final {
public:
    explicit Application(std::vector<std::string> arguments = {}, ApplicationOptions = {});
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    [[nodiscard]] core::Result<void> set_driver(ApplicationDriver);
    [[nodiscard]] core::Result<void> attach(ApplicationAttachment);
    [[nodiscard]] core::Result<int> exec();
    [[nodiscard]] core::Result<void> quit(int exit_code = 0);
    [[nodiscard]] Submission post(Task, Priority = Priority::normal);
    [[nodiscard]] EventLoop& event_loop() noexcept;
    // Same native frame phases for applications and games. An external display
    // source calls step(); headless consumers can use the timer driver or poll.
    [[nodiscard]] FrameLoop& frame_loop() noexcept;
    // Borrow for existing SDK APIs such as SerialQueue/Concurrent. This is the
    // same application pool, not another runtime. Application must outlive use.
    [[nodiscard]] Runtime& workers() noexcept;
    [[nodiscard]] std::stop_token stop_token() const noexcept;
    [[nodiscard]] std::span<const std::string> arguments() const noexcept;
    [[nodiscard]] ApplicationState state() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Implemented by an application translation unit; Core supplies main(), owns
// Application and calls exec() after this function configures its attachments.
[[nodiscard]] core::Result<void> configure_application(Application&);
} // namespace ii::runtime
