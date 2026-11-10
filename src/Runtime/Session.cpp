#include "Session.h"
#include "Runtime.h"
#include <atomic>

namespace ii::runtime {
struct Session::State {
    std::stop_source stop;
    std::atomic<SessionState> phase{SessionState::active};
    ObjectOwner objects;
    TaskScope tasks;
};

Session::Session(Runtime& runtime) : state_(std::make_shared<State>()) { runtime.register_session(state_); }
Session::~Session() {
    const auto result=close();
    if (!result && result.error().code!=ThreadErrorCode::work_failed) std::terminate();
}
ObjectOwner& Session::objects() const { return state_->objects; }
TaskScope::Sender Session::sender() const { return state_->tasks.sender(); }
std::stop_token Session::stop_token() const noexcept { return state_->stop.get_token(); }
void Session::request_close() const {
    // Stop callbacks are user code and may release the last Session handle.
    // Keep the state alive across all callbacks and subsequent cleanup.
    auto state=state_;
    auto active=SessionState::active;
    (void)state->phase.compare_exchange_strong(active,SessionState::closing);
    state->stop.request_stop();
    state->tasks.request_close();
    state->objects.request_close();
}
ThreadResult Session::finish(const std::shared_ptr<State>& state, bool wait) {
    if (is_current(state))
        return std::unexpected(ThreadError{ThreadErrorCode::self_join,{}});
    auto active=SessionState::active;
    (void)state->phase.compare_exchange_strong(active,SessionState::closing);
    state->stop.request_stop();
    state->tasks.request_close();
    state->objects.request_close();
    auto tasks=wait ? state->tasks.close() : state->tasks.try_close();
    if (!tasks && tasks.error().code!=ThreadErrorCode::work_failed) return tasks;
    auto objects=wait ? state->objects.close() : state->objects.try_close();
    if (!objects && objects.error().code!=ThreadErrorCode::work_failed) return objects;
    state->phase.store(SessionState::closed);
    return !tasks ? tasks : objects;
}
ThreadResult Session::close() const { auto state=state_; return finish(state,true); }
ThreadResult Session::try_close() const { auto state=state_; return finish(state,false); }
bool Session::closed() const { return state()==SessionState::closed; }
SessionState Session::state() const noexcept { return state_->phase.load(); }
bool Session::is_current(const std::shared_ptr<State>& state) noexcept {
    return state->tasks.is_current() || state->objects.is_current();
}
} // namespace ii::runtime
