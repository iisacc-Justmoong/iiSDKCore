#include "Application.h"
#include "../Core/Text/Utf8.h"
#include <algorithm>
#include <atomic>
#include <optional>

namespace ii::runtime {
struct Application::Impl final {
    explicit Impl(std::vector<std::string> args, ApplicationOptions options)
        : events(options.event_capacity), frames(events,options.frames), workers(options.workers), arguments(std::move(args)) {}
    // Native driver/attachments are destroyed before their borrowed loop.
    EventLoop events;
    FrameLoop frames;
    Runtime workers;
    TaskScope work;
    std::stop_source stop;
    const std::thread::id owner = std::this_thread::get_id();
    std::vector<std::string> arguments;
    std::vector<ApplicationAttachment> attachments;
    ApplicationDriver driver;
    std::atomic<ApplicationState> phase{ApplicationState::ready};
    std::optional<core::Error> failure;
    std::size_t attempted = 0;
    int exit_code = 0;
    bool started = false, retired = false;

    core::Result<void> on_owner() const {
        if (owner != std::this_thread::get_id())
            return core::fail(core::ErrorCode::wrong_executor,"Application lifecycle requires its owning thread");
        return {};
    }
    core::Result<void> configurable() const {
        if (auto r=on_owner(); !r) return r;
        if (phase.load()!=ApplicationState::ready)
            return core::fail(core::ErrorCode::closed,"Application configuration is closed");
        return {};
    }
    void remember(core::Error error) { if (!failure) failure=std::move(error); }
    void retire() noexcept {
        if (retired) return;
        retired=true;
        phase=ApplicationState::stopping;
        stop.request_stop();
        events.request_stop();
        work.request_close();
        workers.request_stop(ShutdownMode::cancel_pending);
        if (auto r=work.close(); !r) remember({core::ErrorCode::failed,"Application work scope could not close",r.error().cause});
        if (auto r=workers.join(); !r) remember({core::ErrorCode::failed,"Application workers could not join",r.error().cause});
        if (workers.stats().failed) remember({core::ErrorCode::failed,"Application worker callback failed"});
        frames.close();
        while (attempted) {
            auto& attachment=attachments[--attempted];
            try { if (auto r=attachment.deactivate(); !r) remember(std::move(r.error())); }
            catch (...) { remember({core::ErrorCode::failed,"Application attachment deactivation failed",std::current_exception()}); }
        }
        phase=ApplicationState::stopped;
    }
};
Application::Application(std::vector<std::string> args,ApplicationOptions options)
    : impl_(std::make_unique<Impl>(std::move(args),options)) {}
Application::~Application() {
    if (!impl_->on_owner() || (state()!=ApplicationState::ready && state()!=ApplicationState::stopped)) std::terminate();
    impl_->retire();
}
core::Result<void> Application::set_driver(ApplicationDriver driver) {
    if (auto r=impl_->configurable(); !r) return r;
    if (!driver.run || !driver.request_close) return core::fail(core::ErrorCode::invalid_argument,"Application driver requires run and request_close");
    if (impl_->driver.run) return core::fail(core::ErrorCode::already_exists,"Application already has a driver");
    impl_->driver=std::move(driver); return {};
}
core::Result<void> Application::attach(ApplicationAttachment value) {
    if (auto r=impl_->configurable(); !r) return r;
    if (value.name.empty() || !core::valid_utf8(value.name) || value.name.find('\0')!=value.name.npos || !value.activate || !value.deactivate)
        return core::fail(core::ErrorCode::invalid_argument,"Attachment requires a UTF-8 name and paired activation/deactivation");
    if (std::ranges::any_of(impl_->attachments,[&](const auto& existing) { return existing.name==value.name; }))
        return core::fail(core::ErrorCode::already_exists,"Duplicate application attachment name");
    impl_->attachments.push_back(std::move(value)); return {};
}
core::Result<int> Application::exec() {
    if (auto r=impl_->configurable(); !r) return std::unexpected(r.error());
    if (!impl_->driver.run) return core::fail(core::ErrorCode::invalid_argument,"Application has no native driver");
    impl_->phase=ApplicationState::starting;
    try {
        auto result=impl_->driver.run([&]() -> core::Result<void> {
            if (auto r=impl_->on_owner(); !r) return r;
            if (impl_->started) {
                impl_->remember({core::ErrorCode::busy,"Application driver started twice"});
                return std::unexpected(*impl_->failure);
            }
            impl_->started=true; impl_->phase=ApplicationState::running;
            for (auto& attachment:impl_->attachments) {
                // A failed activation can own partial resources; deactivate it too.
                ++impl_->attempted;
                try {
                    if (auto r=attachment.activate(*this); !r) { impl_->remember(r.error()); return r; }
                } catch (...) {
                    impl_->remember({core::ErrorCode::failed,"Application attachment activation failed",std::current_exception()});
                    return std::unexpected(*impl_->failure);
                }
            }
            return {};
        });
        if (!result) impl_->remember(std::move(result.error()));
        if (!impl_->started) impl_->remember({core::ErrorCode::failed,"Application driver returned without starting"});
    } catch (...) { impl_->remember({core::ErrorCode::failed,"Application driver failed",std::current_exception()}); }
    impl_->retire();
    if (impl_->failure) return std::unexpected(*impl_->failure);
    return impl_->exit_code;
}
core::Result<void> Application::quit(int code) {
    if (auto r=impl_->on_owner(); !r) return r;
    if (state()!=ApplicationState::running) return core::fail(core::ErrorCode::closed,"Application is not running");
    impl_->exit_code=code;
    try { return impl_->driver.request_close(); }
    catch (...) { return core::fail(core::ErrorCode::failed,"Application close request failed",std::current_exception()); }
}
Submission Application::post(Task task,Priority priority) { return impl_->work.post(impl_->workers,std::move(task),priority); }
EventLoop& Application::event_loop() noexcept { return impl_->events; }
FrameLoop& Application::frame_loop() noexcept { return impl_->frames; }
Runtime& Application::workers() noexcept { return impl_->workers; }
std::stop_token Application::stop_token() const noexcept { return impl_->stop.get_token(); }
std::span<const std::string> Application::arguments() const noexcept { return impl_->arguments; }
ApplicationState Application::state() const noexcept { return impl_->phase.load(); }
}
