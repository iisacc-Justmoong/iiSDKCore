#pragma once
#include "Runtime.h"
#include <condition_variable>
#include <mutex>
#include <type_traits>
#include <vector>

namespace ii::runtime {
template <class T> struct TimedFrame final {
  Clock::time_point presentation;
  T value;
};
struct LiveStreamStats final {
  std::size_t capacity = 0, pending = 0, high_watermark = 0;
  std::uint64_t delivered = 0, dropped = 0, late = 0, rejected = 0;
};
// A bounded timestamp-ordered jitter buffer with EventLoop timed playout.
// Producer timestamps share steady_clock's epoch. The loop outlives playback.
template <class T> class LiveStream final {
  static_assert(std::is_nothrow_move_constructible_v<T> &&
                std::is_nothrow_move_assignable_v<T>);
  using Handler = std::function<void(TimedFrame<T>)>;
  struct State final {
    explicit State(std::size_t capacity, Clock::duration latency)
        : slots(capacity), lateness(latency) {
      statistics.capacity = capacity;
    }
    std::optional<TimedFrame<T>>
    take(Clock::time_point now, std::optional<Clock::time_point> end = {}) {
      for (;;) {
        if (end && Clock::now() >= *end)
          return {};
        std::optional<TimedFrame<T>> retired;
        bool stale = false;
        {
          std::lock_guard lock(mutex);
          if (closed || !count || slots[head]->presentation > now)
            return {};
          stale =
              core::Time::elapsed(slots[head]->presentation, now) > lateness;
          retired.swap(slots[head]);
          head = (head + 1) % slots.size();
          --count;
          statistics.pending = count;
          if (stale)
            ++statistics.late;
          else
            ++statistics.delivered;
        }
        if (!stale)
          return retired;
      }
    }
    std::mutex mutex;
    std::vector<std::optional<TimedFrame<T>>> slots;
    const Clock::duration lateness;
    std::optional<Clock::time_point> last;
    std::size_t head = 0, count = 0, active = 0;
    bool closed = false;
    Handler handler;
    LiveStreamStats statistics;
    std::mutex control;
    std::condition_variable changed;
    TaskToken token;
    bool started = false, closing = false, cleaned = false;
    std::thread::id closer;
  };
  inline static thread_local const State *current_ = nullptr;
  std::shared_ptr<State> state_;

public:
  explicit LiveStream(
      std::size_t capacity = 256,
      Clock::duration max_lateness = std::chrono::milliseconds(50)) {
    if (!capacity || capacity > 65536 || max_lateness < Clock::duration::zero())
      throw std::invalid_argument("invalid live stream bounds");
    state_ = std::make_shared<State>(capacity, max_lateness);
  }
  ~LiveStream() {
    if (!close())
      std::terminate();
  }
  LiveStream(const LiveStream &) = delete;
  LiveStream &operator=(const LiveStream &) = delete;
  [[nodiscard]] bool push(Clock::time_point presentation, T value) {
    std::optional<TimedFrame<T>> retired;
    {
      std::lock_guard lock(state_->mutex);
      if (state_->closed || (state_->last && presentation < *state_->last)) {
        ++state_->statistics.rejected;
        return false;
      }
      if (state_->count == state_->slots.size()) {
        retired.swap(state_->slots[state_->head]);
        state_->head = (state_->head + 1) % state_->slots.size();
        --state_->count;
        ++state_->statistics.dropped;
      }
      state_->slots[(state_->head + state_->count) % state_->slots.size()]
          .emplace(TimedFrame<T>{presentation, std::move(value)});
      ++state_->count;
      state_->last = presentation;
      state_->statistics.pending = state_->count;
      state_->statistics.high_watermark =
          std::max(state_->statistics.high_watermark, state_->count);
    }
    return true;
  }
  [[nodiscard]] std::optional<TimedFrame<T>>
  take(Clock::time_point now = Clock::now()) {
    return state_->take(now);
  }
  [[nodiscard]] Submission start(EventLoop &loop, RealtimeSchedule schedule,
                                 Handler handler) {
    std::lock_guard control(state_->control);
    {
      std::lock_guard lock(state_->mutex);
      if (state_->closed)
        return {{}, SubmitError::stopped};
      if (state_->started || !handler)
        return {{}, SubmitError::invalid};
      state_->handler.swap(handler);
    }
    const std::weak_ptr<State> weak = state_;
    auto submitted =
        loop.realtime(schedule, [weak](const core::TimeSlice &slice) {
          auto state = weak.lock();
          if (!state)
            return SliceResult::stop;
          {
            std::lock_guard lock(state->mutex);
            if (state->closed)
              return SliceResult::stop;
            ++state->active;
          }
          struct Delivery final {
            State &state;
            const State *previous = current_;
            explicit Delivery(State &value) : state(value) {
              current_ = &state;
            }
            ~Delivery() {
              current_ = previous;
              std::lock_guard lock(state.mutex);
              --state.active;
            }
          } delivery(*state);
          while (!slice.should_yield()) {
            auto frame = state->take(Clock::now(), slice.ends);
            if (!frame)
              return slice.should_yield() ? SliceResult::yield
                                          : SliceResult::complete;
            state->handler(std::move(*frame));
          }
          return SliceResult::yield;
        });
    if (submitted) {
      state_->token = submitted.token;
      state_->started = true;
    } else {
      std::lock_guard lock(state_->mutex);
      state_->handler.swap(handler);
    }
    return submitted;
  }
  [[nodiscard]] ThreadResult close() {
    if (current_ == state_.get())
      return std::unexpected(ThreadError{ThreadErrorCode::busy, {}});
    TaskToken token;
    {
      std::unique_lock control(state_->control);
      if (state_->closing && (state_->closer == std::this_thread::get_id() ||
                              Runtime::is_worker_thread()))
        return std::unexpected(ThreadError{ThreadErrorCode::busy, {}});
      state_->changed.wait(control, [&] { return !state_->closing; });
      if (state_->cleaned)
        return {};
      {
        std::lock_guard lock(state_->mutex);
        if (state_->active && Runtime::is_worker_thread())
          return std::unexpected(ThreadError{ThreadErrorCode::busy, {}});
        state_->closed = true;
      }
      state_->closing = true;
      state_->closer = std::this_thread::get_id();
      token = state_->token;
    }
    (void)token.cancel();
    while (token.pending())
      std::this_thread::yield();
    std::vector<std::optional<TimedFrame<T>>> retired;
    Handler handler;
    {
      std::lock_guard lock(state_->mutex);
      retired.swap(state_->slots);
      state_->handler.swap(handler);
      state_->count = state_->head = 0;
      state_->last.reset();
      state_->statistics.pending = 0;
    }
    handler = {};
    retired.clear();
    {
      std::lock_guard control(state_->control);
      state_->cleaned = true;
      state_->closing = false;
      state_->closer = {};
      state_->changed.notify_all();
    }
    return {};
  }
  [[nodiscard]] LiveStreamStats stats() const {
    std::lock_guard lock(state_->mutex);
    return state_->statistics;
  }
};
} // namespace ii::runtime
