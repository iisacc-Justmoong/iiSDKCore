#pragma once
#include "Time.h"
#include <algorithm>
#include <optional>

namespace ii::core {
class Deadline final {
public:
  using Clock = core::Clock;
  Deadline() = default; // no deadline
  explicit Deadline(Clock::time_point point) : point_(point) {}
  [[nodiscard]] static Deadline after(Clock::duration duration,
                                      Clock::time_point now = Clock::now()) {
    if (duration <= Clock::duration::zero())
      return Deadline(now);
    return Deadline(Time::add(now, duration));
  }
  [[nodiscard]] bool expired(Clock::time_point now = Clock::now()) const {
    return point_ && now >= *point_;
  }
  [[nodiscard]] Clock::duration
  remaining(Clock::time_point now = Clock::now()) const {
    if (!point_)
      return Clock::duration::max();
    if (now >= *point_)
      return Clock::duration::zero();
    return Time::elapsed(now, *point_);
  }

private:
  std::optional<Clock::time_point> point_;
};
} // namespace ii::core
