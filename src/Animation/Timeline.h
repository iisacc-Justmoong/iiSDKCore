#pragma once
#include "../Core/Time/Time.h"
#include <algorithm>
#include <cmath>
#include <concepts>
#include <functional>
#include <utility>
#include <vector>

namespace ii::animation {
enum class Easing { linear, in_quad, out_quad, in_out_quad, smooth };
inline double ease(Easing curve, double position) noexcept {
  const auto p = std::clamp(position, 0.0, 1.0);
  switch (curve) {
  case Easing::in_quad:
    return p * p;
  case Easing::out_quad:
    return p * (2 - p);
  case Easing::in_out_quad:
    return p < 0.5 ? 2 * p * p : 1 - 2 * (1 - p) * (1 - p);
  case Easing::smooth:
    return p * p * (3 - 2 * p);
  default:
    return p;
  }
}
template <class T> struct Keyframe final {
  core::Clock::duration time{};
  T value;
  Easing easing = Easing::linear;
};
template <class T> class Keyframes final {
public:
  using Interpolate = std::function<T(const T &, const T &, double)>;
  explicit Keyframes(std::vector<Keyframe<T>> keys,
                     Interpolate interpolate = {})
      : keys_(std::move(keys)), interpolate_(std::move(interpolate)) {
    if (keys_.empty() || keys_.front().time < core::Clock::duration::zero())
      throw std::invalid_argument("Keyframes require a nonnegative first key");
    for (std::size_t i = 1; i < keys_.size(); ++i)
      if (keys_[i].time <= keys_[i - 1].time)
        throw std::invalid_argument("Keyframe times must strictly increase");
    if (!interpolate_) {
      if constexpr (std::floating_point<T>)
        interpolate_ = [](T a, T b, double p) {
          return static_cast<T>(std::lerp(a, b, p));
        };
      else
        throw std::invalid_argument(
            "This value type requires an interpolation function");
    }
  }
  [[nodiscard]] T sample(core::Clock::duration time) const {
    if (time <= keys_.front().time)
      return keys_.front().value;
    if (time >= keys_.back().time)
      return keys_.back().value;
    auto right = std::ranges::upper_bound(keys_, time, {}, &Keyframe<T>::time);
    const auto &left = *std::prev(right);
    const double p =
        std::chrono::duration<double>(time - left.time).count() /
        std::chrono::duration<double>(right->time - left.time).count();
    return interpolate_(left.value, right->value, ease(left.easing, p));
  }
  [[nodiscard]] core::Clock::duration duration() const noexcept {
    return keys_.back().time;
  }

private:
  std::vector<Keyframe<T>> keys_;
  Interpolate interpolate_;
};
// All callbacks in a timeline execute in one caller's domain. Parallel means
// overlapping animation time; CPU-parallel independent work uses Runtime.
class Timeline final {
public:
  using Sample = std::function<void(double)>;
  void parallel(core::Clock::duration start, core::Clock::duration duration,
                Sample sample, Easing easing = Easing::linear) {
    if (start < core::Clock::duration::zero() ||
        duration < core::Clock::duration::zero() || !sample ||
        start > core::Clock::duration::max() - duration)
      throw std::invalid_argument("Invalid animation track");
    tracks_.push_back({start, duration, std::move(sample), easing});
    duration_ = std::max(duration_, start + duration);
  }
  void append(core::Clock::duration duration, Sample sample,
              Easing easing = Easing::linear) {
    parallel(duration_, duration, std::move(sample), easing);
  }
  void pause(core::Clock::duration duration) {
    if (duration < core::Clock::duration::zero() ||
        duration_ > core::Clock::duration::max() - duration)
      throw std::invalid_argument("Invalid timeline pause");
    duration_ += duration;
  }
  void sample(core::Clock::duration time) const {
    for (const auto &track : tracks_)
      if (time >= track.start) {
        const double p =
            track.duration == core::Clock::duration::zero()
                ? 1.0
                : std::chrono::duration<double>(time - track.start).count() /
                      std::chrono::duration<double>(track.duration).count();
        track.sample(ease(track.easing, p));
      }
  }
  [[nodiscard]] core::Clock::duration duration() const noexcept {
    return duration_;
  }

private:
  struct Track final {
    core::Clock::duration start, duration;
    Sample sample;
    Easing easing;
  };
  std::vector<Track> tracks_;
  core::Clock::duration duration_{};
};
} // namespace ii::animation
