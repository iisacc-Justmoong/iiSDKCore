#pragma once
#include "EventLoop.h"
#include "../Core/Result/Result.h"
#include <type_traits>

namespace ii::runtime {
struct HostLoopOptions final {
    // Native readiness waits run separately; callbacks still dispatch on the
    // attaching thread. Disable only for hosts that explicitly drive I/O.
    bool monitor_io = true;
};
struct HostLoopStats final {
    bool connected = false;
    std::uint64_t wake_requests = 0, wake_signals = 0, io_waits = 0;
};

// Binds an existing EventLoop to an external event host, without native handles
// or an upper-framework dependency. Construct/close/destroy on the host owner.
// wake must signal the host, never dispatch synchronously or wait for its reply.
class HostLoop final {
public:
    template<class Wake>
        requires std::is_nothrow_invocable_r_v<void, Wake&> && std::is_copy_constructible_v<Wake>
    HostLoop(EventLoop& events, Wake wake, HostLoopOptions options = {})
        : state_(connect(events, Task(std::move(wake)), options)) {}
    ~HostLoop();
    HostLoop(const HostLoop&) = delete;
    HostLoop& operator=(const HostLoop&) = delete;
    HostLoop(HostLoop&&) = delete;
    HostLoop& operator=(HostLoop&&) = delete;
    // Detach is a barrier for in-flight wake calls and the readiness waiter.
    // It leaves EventLoop ownership/state with the caller; close is idempotent.
    [[nodiscard]] core::Result<void> close();
    [[nodiscard]] HostLoopStats stats() const;
    [[nodiscard]] static bool is_current_thread() noexcept;
private:
    friend class EventLoop;
    struct State;
    std::shared_ptr<State> state_;
    static std::shared_ptr<State> connect(EventLoop&, Task, HostLoopOptions);
    static core::Result<void> disconnect(std::shared_ptr<State>);
};
} // namespace ii::runtime
