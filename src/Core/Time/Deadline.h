#pragma once
#include "../../Runtime/EventLoop.h"
#include <algorithm>

namespace ii::core {
class Deadline final {
public:
    using Clock=runtime::Clock;
    Deadline() = default; // no deadline
    explicit Deadline(Clock::time_point point) : point_(point) {}
    [[nodiscard]] static Deadline after(Clock::duration duration, Clock::time_point now=Clock::now()) {
        if (duration<=Clock::duration::zero()) return Deadline(now);
        if (now.time_since_epoch()>Clock::duration::max()-duration) return Deadline(Clock::time_point::max());
        return Deadline(now+duration);
    }
    [[nodiscard]] bool expired(Clock::time_point now=Clock::now()) const { return point_ && now>=*point_; }
    [[nodiscard]] Clock::duration remaining(Clock::time_point now=Clock::now()) const {
        if (!point_) return Clock::duration::max();
        if (now>=*point_) return Clock::duration::zero();
        if (now.time_since_epoch()<Clock::duration::zero() && point_->time_since_epoch()>Clock::duration::max()+now.time_since_epoch())
            return Clock::duration::max();
        return *point_-now;
    }
private:
    std::optional<Clock::time_point> point_;
};
} // namespace ii::core
