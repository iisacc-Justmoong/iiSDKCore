#pragma once
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace ii::core {
using Clock = std::chrono::steady_clock;
struct PeriodPosition final {
  Clock::time_point release, next;
  std::uint64_t skipped = 0;
};
// Monotonic scheduling arithmetic, including the full signed epoch range.
class Time final {
public:
  static Clock::time_point add(Clock::time_point point,
                               Clock::duration delta) noexcept {
    using Rep = Clock::duration::rep;
    const auto value = point.time_since_epoch().count(), change = delta.count();
    if (change > 0 && value > std::numeric_limits<Rep>::max() - change)
      return Clock::time_point::max();
    if (change < 0 && value < std::numeric_limits<Rep>::min() - change)
      return Clock::time_point::min();
    return Clock::time_point(Clock::duration(value + change));
  }
  static Clock::duration elapsed(Clock::time_point from,
                                 Clock::time_point to) noexcept {
    if (to <= from)
      return {};
    using Rep = Clock::duration::rep;
    using Unsigned = std::make_unsigned_t<Rep>;
    const auto count = static_cast<Unsigned>(to.time_since_epoch().count()) -
                       static_cast<Unsigned>(from.time_since_epoch().count());
    if (count > static_cast<Unsigned>(std::numeric_limits<Rep>::max()))
      return Clock::duration::max();
    return Clock::duration(static_cast<Rep>(count));
  }
  static PeriodPosition period(Clock::time_point origin,
                               Clock::duration interval,
                               Clock::time_point now) {
    if (interval <= Clock::duration::zero())
      throw std::invalid_argument("positive period required");
    if (now < origin)
      return {origin, add(origin, interval), 0};
    using Rep = Clock::duration::rep;
    using Unsigned = std::make_unsigned_t<Rep>;
    static_assert(std::is_integral_v<Rep> &&
                  sizeof(Rep) <= sizeof(std::uint64_t));
    const auto distance =
        static_cast<Unsigned>(now.time_since_epoch().count()) -
        static_cast<Unsigned>(origin.time_since_epoch().count());
    const auto ticks = static_cast<Unsigned>(interval.count());
    const auto release =
        add(now, -Clock::duration(static_cast<Rep>(distance % ticks)));
    return {release, add(release, interval),
            static_cast<std::uint64_t>(distance / ticks)};
  }
};
// Borrowed during one callback. Yield before ends; C++ code is cooperative.
struct TimeSlice final {
  Clock::time_point release, deadline, started, ends;
  Clock::duration remaining_runtime{};
  [[nodiscard]] Clock::duration
  remaining(Clock::time_point now = Clock::now()) const noexcept {
    return Time::elapsed(now, ends);
  }
  [[nodiscard]] bool
  should_yield(Clock::time_point now = Clock::now()) const noexcept {
    return now >= ends;
  }
};
} // namespace ii::core
