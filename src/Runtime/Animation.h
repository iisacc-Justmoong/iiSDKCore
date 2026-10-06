#pragma once
#include "../Animation/Timeline.h"
#include "../Core/Result/Result.h"
#include "TaskScope.h"

namespace ii::runtime {
enum class AnimationState {
  stopped,
  playing,
  paused,
  completed,
  failed,
  closed
};
struct AnimationOptions final {
  Clock::duration interval = std::chrono::milliseconds(16);
  std::uint32_t iterations = 1; // 0 repeats until explicitly closed
  bool alternate = false;
  Task finished;
};
// The loop outlives playback. Control calls are serialized and callbacks must
// not recursively control or destroy playback; close reports self_join there.
class Animation final {
public:
  Animation(EventLoop &loop, animation::Timeline timeline,
            AnimationOptions options = {});
  ~Animation();
  Animation(const Animation &) = delete;
  Animation &operator=(const Animation &) = delete;
  [[nodiscard]] core::Result<void> play();
  [[nodiscard]] core::Result<void> pause();
  [[nodiscard]] core::Result<void> seek(Clock::duration position);
  [[nodiscard]] ThreadResult close();
  [[nodiscard]] AnimationState state() const;
  [[nodiscard]] std::exception_ptr failure() const;

private:
  struct State;
  std::shared_ptr<State> state_;
};
} // namespace ii::runtime
