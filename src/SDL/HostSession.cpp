#include "HostSession.h"

namespace ii::sdl {
namespace {
struct Guard { bool& flag; explicit Guard(bool& value):flag(value){flag=true;} ~Guard(){flag=false;} };
}
HostSession::HostSession(HostHooks hooks):hooks_(std::move(hooks)) {
    if (!hooks_.initialize || !hooks_.pump || !hooks_.shutdown) throw std::invalid_argument("HostSession requires native hooks");
}
HostSession::~HostSession() {
    if (active_ && owner_!=runtime::Thread::current_id()) std::terminate();
    if (active_) (void)close();
}
core::Result<void> HostSession::open() {
    if (owner_!=runtime::Thread::current_id()) return core::fail(core::ErrorCode::wrong_executor,"Host must run on its owning thread");
    if (active_ || inside_) return core::fail(core::ErrorCode::busy,"Host already active or reentrant");
    Guard guard(inside_);
    try { auto result=hooks_.initialize(); if (result) active_=true; return result; }
    catch (...) { return core::fail(core::ErrorCode::failed,"Host initialization failed",std::current_exception()); }
}
core::Result<bool> HostSession::step(runtime::EventLoop& loop,runtime::PumpBudget budget) {
    if (owner_!=runtime::Thread::current_id()) return core::fail(core::ErrorCode::wrong_executor,"Host must run on its owning thread");
    if (!active_) return core::fail(core::ErrorCode::closed,"Host is not active");
    if (inside_) return core::fail(core::ErrorCode::busy,"Reentrant host pump");
    Guard guard(inside_);
    try { auto result=hooks_.pump(); if (!result) return result; loop.poll(budget); return *result; }
    catch (...) { return core::fail(core::ErrorCode::failed,"Host pump failed",std::current_exception()); }
}
core::Result<void> HostSession::close() {
    if (owner_!=runtime::Thread::current_id()) return core::fail(core::ErrorCode::wrong_executor,"Host must run on its owning thread");
    if (inside_) return core::fail(core::ErrorCode::busy,"Reentrant host shutdown");
    if (!active_) return {};
    active_=false;
    Guard guard(inside_);
    try { return hooks_.shutdown(); }
    catch (...) { return core::fail(core::ErrorCode::failed,"Host shutdown failed",std::current_exception()); }
}
} // namespace ii::sdl
