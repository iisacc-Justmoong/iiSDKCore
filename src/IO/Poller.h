#pragma once
#include "Channel.h"

namespace ii::io::detail {
struct WatchEntry final { Channel channel; Interest interest; };

// Internal native wait boundary. Prepared leases protect handles from reuse;
// cancellation waits for their epoch to finish before freeing the task slot.
class ReadinessPoller final {
public:
    explicit ReadinessPoller(std::size_t capacity);
    ~ReadinessPoller();
    void prepare(std::span<const WatchEntry>, std::chrono::milliseconds timeout);
    void wait();
    void wake() noexcept;
    void synchronize();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ii::io::detail
