#pragma once
#include "../../Runtime/EventLoop.h"
#include <atomic>
#include <utility>

namespace ii::core {
template<class T> struct Event { std::uint64_t sequence; runtime::Clock::time_point time; T payload; };
// Value envelope for query payloads; no secondary signal/subscriber system.
class EventSequence final {
public:
    template<class T> [[nodiscard]] Event<std::decay_t<T>> make(T&& payload) {
        return {sequence_.fetch_add(1,std::memory_order_relaxed)+1, runtime::Clock::now(), std::forward<T>(payload)};
    }
private:
    std::atomic<std::uint64_t> sequence_{0};
};
} // namespace ii::core
