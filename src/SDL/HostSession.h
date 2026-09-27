#pragma once
#include "../Core/Result/Result.h"
#include "../Runtime/Thread.h"

namespace ii::sdl {
struct HostHooks {
    std::function<core::Result<void>()> initialize;
    std::function<core::Result<bool>()> pump; // false: native host requests exit
    std::function<core::Result<void>()> shutdown;
};
// SDL2/SDL3 embedding boundary. Caller supplies actual native SDK operations;
// no SDK is linked and no absent backend is reported as initialized.
class HostSession final {
public:
    explicit HostSession(HostHooks hooks);
    ~HostSession();
    HostSession(const HostSession&)=delete;
    HostSession& operator=(const HostSession&)=delete;
    [[nodiscard]] core::Result<void> open();
    [[nodiscard]] core::Result<bool> step(runtime::EventLoop& loop,runtime::PumpBudget budget={});
    [[nodiscard]] core::Result<void> close();
private:
    HostHooks hooks_;
    const runtime::Thread::Id owner_=runtime::Thread::current_id();
    bool active_=false,inside_=false;
};
} // namespace ii::sdl
