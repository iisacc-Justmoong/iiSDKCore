#pragma once
#include "../Core/Result/Result.h"
#include <cstdint>
#include <atomic>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace ii::vulkan {
// Correlates application submission values with a backend timeline semaphore.
// The callback reads the actual driver counter; issue() never signals the GPU.
class Timeline final {
public:
    using Counter=std::function<core::Result<std::uint64_t>()>;
    explicit Timeline(Counter counter):counter_(std::move(counter)) {
        if (!counter_) throw std::invalid_argument("Timeline requires a backend counter");
    }
    [[nodiscard]] core::Result<std::uint64_t> issue() {
        std::lock_guard lock(mutex_);
        if (issued_==std::numeric_limits<std::uint64_t>::max()) return core::fail(core::ErrorCode::out_of_range,"Timeline exhausted");
        return ++issued_;
    }
    [[nodiscard]] core::Result<std::uint64_t> poll() {
        if (polling_.test_and_set(std::memory_order_acquire)) return core::fail(core::ErrorCode::busy,"Timeline polling already in progress");
        struct PollGuard final { std::atomic_flag& flag; ~PollGuard(){flag.clear(std::memory_order_release);} } guard{polling_};
        core::Result<std::uint64_t> value;
        try { value=counter_(); }
        catch (...) { return core::fail(core::ErrorCode::failed,"Timeline backend failed",std::current_exception()); }
        if (!value) return value;
        std::lock_guard lock(mutex_);
        if (*value<completed_ || *value>issued_) return core::fail(core::ErrorCode::out_of_range,"Backend counter outside submitted timeline");
        completed_=*value; return completed_;
    }
    [[nodiscard]] core::Result<bool> ready(std::uint64_t ticket) const {
        std::lock_guard lock(mutex_);
        if (!ticket || ticket>issued_) return core::fail(core::ErrorCode::invalid_argument,"Unknown timeline ticket");
        return ticket<=completed_;
    }
private:
    Counter counter_;
    mutable std::mutex mutex_;
    std::atomic_flag polling_=ATOMIC_FLAG_INIT;
    std::uint64_t issued_=0,completed_=0;
};
} // namespace ii::vulkan
