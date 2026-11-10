#pragma once
#include "Channel.h"

namespace ii::io::detail {
struct WatchEntry final {
    Channel channel;
    Interest interest;
    std::optional<std::chrono::milliseconds> polling_interval;
};

// Internal native wait boundary. Prepared leases protect handles from reuse;
// cancellation waits for their epoch to finish before freeing the task slot.
class ReadinessPoller final {
public:
    explicit ReadinessPoller(std::size_t capacity);
    ~ReadinessPoller();
    // Optional already-owned native process-completion signal; the caller
    // retains it until wait() returns. -1 means no additional signal.
    void prepare(std::span<const WatchEntry>, std::chrono::milliseconds timeout,
                 std::intptr_t native_signal = -1);
    void wait();
    void wake() noexcept;
    // Sticky even before prepare(), so an asynchronous cancellation cannot
    // be lost between the readiness check and the native wait.
    void wake_persistent() noexcept;
    void synchronize();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ii::io::detail
