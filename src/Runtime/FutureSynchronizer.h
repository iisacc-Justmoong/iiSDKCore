#pragma once
#include "Future.h"

namespace ii::runtime {
// Explicit group wait: destruction never blocks a worker or a host UI thread.
// One caller owns group mutation; the contained futures remain thread-safe.
template<class T> class FutureSynchronizer final {
public:
    explicit FutureSynchronizer(std::size_t capacity = 1024) : capacity_(capacity) {}
    [[nodiscard]] core::Result<void> add(Future<T> future) {
        if (!future.valid()) return core::fail(core::ErrorCode::invalid_argument, "Invalid future in synchronizer");
        if (futures_.size() == capacity_) return core::fail(core::ErrorCode::full, "Future synchronizer is full");
        futures_.push_back(std::move(future)); return {};
    }
    void cancel() const { for (const auto& future : futures_) (void)future.cancel(); }
    void clear() { futures_.clear(); }
    [[nodiscard]] std::size_t size() const noexcept { return futures_.size(); }
    [[nodiscard]] FutureWait wait() const {
        for (const auto& future : futures_) {
            const auto result = future.wait(); if (result != FutureWait::ready) return result;
        }
        return FutureWait::ready;
    }
    [[nodiscard]] FutureWait wait_for(Clock::duration duration) const {
        const auto start = Clock::now();
        for (const auto& future : futures_) {
            const auto elapsed = Clock::now() - start;
            const auto remaining = elapsed < duration ? duration - elapsed : Clock::duration::zero();
            const auto result = future.wait_for(remaining);
            if (result != FutureWait::ready) return result;
        }
        return FutureWait::ready;
    }
private:
    std::size_t capacity_;
    std::vector<Future<T>> futures_;
};
} // namespace ii::runtime
