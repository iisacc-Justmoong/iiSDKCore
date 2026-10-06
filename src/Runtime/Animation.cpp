#include "Animation.h"
#include <mutex>

namespace ii::runtime {
struct Animation::State final : std::enable_shared_from_this<State> {
  EventLoop &loop;
  animation::Timeline timeline;
  AnimationOptions options;
  mutable std::mutex mutex;
  AnimationState phase = AnimationState::stopped;
  Clock::time_point origin{};
  Clock::duration position{};
  TaskToken pending;
  std::uint64_t generation = 0;
  std::exception_ptr error;
  TaskScope lifetime;
  State(EventLoop &target, animation::Timeline value, AnimationOptions settings)
      : loop(target), timeline(std::move(value)), options(std::move(settings)),
        lifetime([this] {
          timeline = animation::Timeline{};
          options.finished = nullptr;
          std::lock_guard lock(mutex);
          phase = AnimationState::closed;
        }) {}
  Submission schedule(Clock::time_point deadline) {
    auto result = lifetime.at(loop, deadline,
                              [weak = weak_from_this(), epoch = generation] {
                                if (auto self = weak.lock())
                                  self->tick(epoch);
                              });
    pending = result.token;
    return result;
  }
  bool render() {
    const auto length = timeline.duration();
    std::uint64_t cycle = 0;
    Clock::duration local{};
    bool complete = length == Clock::duration::zero();
    if (length > Clock::duration::zero()) {
      cycle = static_cast<std::uint64_t>(position / length);
      local = position % length;
      complete = options.iterations && cycle >= options.iterations;
      if (complete) {
        cycle = options.iterations - 1;
        local = length;
      }
    }
    if (options.alternate && cycle % 2)
      local = length - local;
    timeline.sample(local);
    return complete;
  }
  void tick(std::uint64_t epoch) {
    std::lock_guard lock(mutex);
    if (phase != AnimationState::playing || epoch != generation)
      return;
    try {
      const auto now = Clock::now();
      position = core::Time::elapsed(origin, now);
      if (render()) {
        phase = AnimationState::completed;
        if (options.finished)
          options.finished();
      } else {
        const auto next =
            core::Time::period(origin, options.interval, Clock::now()).next;
        if (next <= Clock::now() || !schedule(next))
          phase = AnimationState::stopped;
      }
    } catch (...) {
      error = std::current_exception();
      phase = AnimationState::failed;
    }
  }
  template <class Operation> core::Result<void> control(Operation operation) {
    if (lifetime.is_current())
      return core::fail(core::ErrorCode::busy, "Recursive animation control");
    core::Result<void> result;
    const auto admitted = lifetime.run([&] {
      std::lock_guard lock(mutex);
      result = operation();
    });
    if (admitted != SubmitError::none)
      return core::fail(core::ErrorCode::closed, "Animation is closed");
    return result;
  }
};
Animation::Animation(EventLoop &loop, animation::Timeline timeline,
                     AnimationOptions options) {
  if (options.interval <= Clock::duration::zero())
    throw std::invalid_argument("Positive animation interval required");
  state_ =
      std::make_shared<State>(loop, std::move(timeline), std::move(options));
}
Animation::~Animation() {
  auto result = close();
  if (!result && result.error().code != ThreadErrorCode::work_failed)
    std::terminate();
}
core::Result<void> Animation::play() {
  auto &s = *state_;
  return s.control([&]() -> core::Result<void> {
    if (s.phase == AnimationState::playing)
      return {};
    if (s.phase == AnimationState::completed ||
        s.phase == AnimationState::failed) {
      s.position = {};
      s.error = {};
    }
    ++s.generation;
    s.origin = core::Time::add(Clock::now(), -s.position);
    auto result = s.schedule(Clock::now());
    if (!result)
      return core::fail(result.error == SubmitError::full
                            ? core::ErrorCode::full
                            : core::ErrorCode::closed,
                        "Animation timer admission failed");
    s.phase = AnimationState::playing;
    return {};
  });
}
core::Result<void> Animation::pause() {
  auto &s = *state_;
  return s.control([&]() -> core::Result<void> {
    if (s.phase == AnimationState::playing) {
      s.position = core::Time::elapsed(s.origin, Clock::now());
      s.phase = AnimationState::paused;
      ++s.generation;
      (void)s.pending.cancel();
    }
    return {};
  });
}
core::Result<void> Animation::seek(Clock::duration position) {
  auto &s = *state_;
  if (position < Clock::duration::zero())
    return core::fail(core::ErrorCode::invalid_argument,
                      "Negative animation seek");
  return s.control([&]() -> core::Result<void> {
    s.position = position;
    s.origin = core::Time::add(Clock::now(), -position);
    try {
      if (s.render()) {
        s.phase = AnimationState::completed;
        (void)s.pending.cancel();
      }
    } catch (...) {
      s.error = std::current_exception();
      s.phase = AnimationState::failed;
      (void)s.pending.cancel();
      return core::fail(core::ErrorCode::failed, "Animation sample failed",
                        s.error);
    }
    return {};
  });
}
ThreadResult Animation::close() { return state_->lifetime.close(); }
AnimationState Animation::state() const {
  if (state_->lifetime.is_current())
    return state_->phase;
  std::lock_guard lock(state_->mutex);
  return state_->phase;
}
std::exception_ptr Animation::failure() const {
  if (state_->lifetime.is_current())
    return state_->error;
  std::lock_guard lock(state_->mutex);
  return state_->error;
}
} // namespace ii::runtime
