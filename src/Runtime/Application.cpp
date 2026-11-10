#include "Application.h"
#include "../Core/Text/Utf8.h"
#include <algorithm>
#include <atomic>
#include <optional>
#include <stdexcept>

namespace ii::runtime {
struct Application::Impl final {
    struct Registration final { ApplicationAttachment attachment; bool before_driver; };
    explicit Impl(std::vector<std::string> args, ApplicationOptions options)
        : events(options.event_capacity), frame_options(options.frames), workers(options.workers), arguments(std::move(args)) {}
    // Native driver/attachments are destroyed before their borrowed loop.
    EventLoop events;
    FrameLoopOptions frame_options;
    std::unique_ptr<FrameLoop> frames;
    Runtime workers;
    TaskScope work;
    std::stop_source stop;
    const std::thread::id owner = std::this_thread::get_id();
    std::vector<std::string> arguments;
    std::vector<Registration> attachments;
    ApplicationDriver driver;
    std::atomic<ApplicationState> phase{ApplicationState::ready};
    std::optional<core::Error> failure;
    std::size_t attempted_before_driver = 0, attempted_driver_ready = 0;
    int exit_code = 0;
    bool started = false, retired = false, configuring = false, rolling_back = false;
    bool driver_entered = false, activating = false, pending_driver_stop = false;

    core::Result<void> on_owner() const {
        if (owner != std::this_thread::get_id())
            return core::fail(core::ErrorCode::wrong_executor,"Application lifecycle requires its owning thread");
        return {};
    }
    core::Result<void> configurable() const {
        if (auto r=on_owner(); !r) return r;
        if (phase.load()!=ApplicationState::ready)
            return core::fail(core::ErrorCode::closed,"Application configuration is closed");
        if (rolling_back)
            return core::fail(core::ErrorCode::busy,"Application configuration is rolling back");
        return {};
    }
    void remember(core::Error error) { if (!failure) failure=std::move(error); }
    core::Result<void> append(ApplicationAttachment value,bool before_driver) {
        if (auto r=configurable(); !r) return r;
        if (value.name.empty() || !core::valid_utf8(value.name) || value.name.find('\0')!=value.name.npos || !value.activate || !value.deactivate)
            return core::fail(core::ErrorCode::invalid_argument,"Attachment requires a UTF-8 name and paired activation/deactivation");
        if (std::ranges::any_of(attachments,[&](const auto& existing) { return existing.attachment.name==value.name; }))
            return core::fail(core::ErrorCode::already_exists,"Duplicate application attachment name");
        attachments.push_back({std::move(value),before_driver}); return {};
    }
    core::Result<void> activate(Application& app,bool before_driver) {
        for (std::size_t i=0;i<attachments.size();++i) {
            auto& entry=attachments[i];
            if (entry.before_driver!=before_driver) continue;
            (before_driver ? attempted_before_driver : attempted_driver_ready)=i+1;
            try {
                if (auto result=entry.attachment.activate(app); !result) { remember(result.error()); return result; }
            } catch (...) {
                remember({core::ErrorCode::failed,"Application attachment activation failed",std::current_exception()});
                return std::unexpected(*failure);
            }
            if (!before_driver && pending_driver_stop) return {};
        }
        return {};
    }
    void deactivate(bool before_driver,std::size_t boundary) noexcept {
        while (boundary) {
            auto& entry=attachments[--boundary];
            if (entry.before_driver!=before_driver) continue;
            try { if (auto result=entry.attachment.deactivate(); !result) remember(std::move(result.error())); }
            catch (...) { remember({core::ErrorCode::failed,"Application attachment deactivation failed",std::current_exception()}); }
        }
    }
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
        if (frames) frames->close();
        deactivate(false,attempted_driver_ready);
        deactivate(true,attempted_before_driver);
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
    return impl_->append(std::move(value),false);
}
core::Result<void> Application::attach_before_driver(ApplicationAttachment value) {
    return impl_->append(std::move(value),true);
}
core::Result<void> Application::configure_atomically(
        std::function<core::Result<void>(Application&)> configure) {
    if (auto r=impl_->configurable(); !r) return r;
    if (impl_->configuring) return core::fail(core::ErrorCode::busy,"Application configuration is already in progress");
    if (!configure) return core::fail(core::ErrorCode::invalid_argument,"Application configuration callback is empty");
    impl_->configuring=true;
    struct Guard final {
        Impl& state;
        std::size_t attachments;
        bool had_driver, committed=false;
        ~Guard() {
            if (!committed) {
                state.rolling_back=true;
                while (state.attachments.size()>attachments) state.attachments.pop_back();
                if (!had_driver) state.driver={};
                state.rolling_back=false;
            }
            state.configuring=false;
        }
    } guard{*impl_,impl_->attachments.size(),bool(impl_->driver.run)};
    try {
        auto result=configure(*this);
        guard.committed=bool(result);
        return result;
    } catch (...) {
        return core::fail(core::ErrorCode::failed,"Application configuration callback failed",std::current_exception());
    }
}
core::Result<int> Application::exec() {
    if (auto r=impl_->configurable(); !r) return std::unexpected(r.error());
    if (impl_->configuring) return core::fail(core::ErrorCode::busy,"Application configuration is in progress");
    if (!impl_->driver.run) return core::fail(core::ErrorCode::invalid_argument,"Application has no native driver");
    impl_->phase=ApplicationState::starting;
    try {
        if (auto early=impl_->activate(*this,true); early) {
            impl_->driver_entered=true;
            auto result=impl_->driver.run([&]() -> core::Result<void> {
                if (auto r=impl_->on_owner(); !r) return r;
                if (impl_->retired || impl_->pending_driver_stop) {
                    core::Error stopped{core::ErrorCode::closed,"Application driver has already stopped"};
                    impl_->remember(stopped);
                    return std::unexpected(std::move(stopped));
                }
                if (impl_->started) {
                    impl_->remember({core::ErrorCode::busy,"Application driver started twice"});
                    return std::unexpected(*impl_->failure);
                }
                impl_->started=true; impl_->phase=ApplicationState::running;
                impl_->activating=true;
                struct ActivationGuard final {
                    Impl& state;
                    ~ActivationGuard() {
                        state.activating=false;
                        if (state.pending_driver_stop) { state.pending_driver_stop=false; state.retire(); }
                    }
                } guard{*impl_};
                return impl_->activate(*this,false);
            });
            if (!result) impl_->remember(std::move(result.error()));
            if (!impl_->started) impl_->remember({core::ErrorCode::failed,"Application driver returned without starting"});
        }
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
core::Result<void> Application::driver_stopped() {
    if (auto r=impl_->on_owner(); !r) return r;
    if (!impl_->driver_entered) return core::fail(core::ErrorCode::busy,"Application driver has not entered");
    if (impl_->retired) return {};
    if (impl_->activating) { impl_->pending_driver_stop=true; return {}; }
    impl_->retire();
    return {};
}
Submission Application::post(Task task,Priority priority) { return impl_->work.post(impl_->workers,std::move(task),priority); }
EventLoop& Application::event_loop() noexcept { return impl_->events; }
FrameLoop& Application::frame_loop() {
    if (!impl_->on_owner()) throw std::logic_error("Application frame loop requires its owning thread");
    if (!impl_->frames) {
        auto frames=std::make_unique<FrameLoop>(impl_->events,impl_->frame_options);
        if (impl_->retired) frames->close();
        impl_->frames=std::move(frames);
    }
    return *impl_->frames;
}
bool Application::has_frame_loop() const noexcept { return bool(impl_->frames); }
Runtime& Application::workers() noexcept { return impl_->workers; }
std::stop_token Application::stop_token() const noexcept { return impl_->stop.get_token(); }
std::span<const std::string> Application::arguments() const noexcept { return impl_->arguments; }
ApplicationState Application::state() const noexcept { return impl_->phase.load(); }
}
