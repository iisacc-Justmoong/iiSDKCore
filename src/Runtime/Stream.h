#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace ii::runtime {

enum class OverflowPolicy { reject, drop_oldest };
struct StreamStats {
    std::size_t capacity;
    std::size_t size;
    std::size_t high_watermark;
    std::uint64_t dropped;
    std::uint64_t rejected;
};

// Bounded MPMC storage. T owns its payload; use values or reference_wrapper
// when another owner explicitly guarantees a longer lifetime.
template<class T>
class Stream final {
    static_assert(std::is_nothrow_move_constructible_v<T> &&
                  std::is_nothrow_move_assignable_v<T>,
                  "Stream payloads must support nonthrowing moves");
public:
    explicit Stream(std::size_t capacity, OverflowPolicy policy = OverflowPolicy::reject)
        : slots_(capacity), policy_(policy) {
        if (capacity == 0) throw std::invalid_argument("Stream capacity must be positive");
        if (policy != OverflowPolicy::reject && policy != OverflowPolicy::drop_oldest)
            throw std::invalid_argument("Invalid stream overflow policy");
    }
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    [[nodiscard]] bool try_push(T value) {
        std::lock_guard lock(mutex_);
        if (closed_ || (size_ == slots_.size() && policy_ == OverflowPolicy::reject)) {
            ++rejected_;
            return false;
        }
        if (size_ == slots_.size()) {
            slots_[head_].reset();
            head_ = (head_ + 1) % slots_.size();
            --size_;
            ++dropped_;
        }
        slots_[(head_ + size_) % slots_.size()].emplace(std::move(value));
        ++size_;
        if (size_ > high_watermark_) high_watermark_ = size_;
        return true;
    }

    [[nodiscard]] bool try_pop(T& output) {
        std::lock_guard lock(mutex_);
        if (size_ == 0) return false;
        output = std::move(*slots_[head_]);
        slots_[head_].reset();
        head_ = (head_ + 1) % slots_.size();
        --size_;
        return true;
    }
    void close() { std::lock_guard lock(mutex_); closed_ = true; }
    [[nodiscard]] bool drained() const {
        std::lock_guard lock(mutex_);
        return closed_ && size_ == 0;
    }
    [[nodiscard]] StreamStats stats() const {
        std::lock_guard lock(mutex_);
        return {slots_.size(), size_, high_watermark_, dropped_, rejected_};
    }
private:
    mutable std::mutex mutex_;
    std::vector<std::optional<T>> slots_;
    OverflowPolicy policy_;
    std::size_t head_ = 0, size_ = 0, high_watermark_ = 0;
    std::uint64_t dropped_ = 0, rejected_ = 0;
    bool closed_ = false;
};

} // namespace ii::runtime
