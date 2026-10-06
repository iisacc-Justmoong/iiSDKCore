#pragma once
#include "../../Runtime/Runtime.h"
#include "../Result/Result.h"
#include <algorithm>
#include <concepts>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace ii::core {
template <class T> struct ParallelResult final {
  std::shared_future<Result<std::vector<T>>> completion;
  io::CancellationSource stop;
  std::vector<runtime::TaskToken> tokens;
  void cancel() const {
    stop.cancel();
    for (const auto &token : tokens)
      (void)token.cancel();
  }
};
namespace detail {
template <class In, class Out> struct ParallelState final {
  std::vector<In> input;
  std::vector<std::optional<Out>> output;
  io::CancellationToken stop;
  std::mutex mutex;
  std::size_t pending = 0;
  bool sealed = false, settled = false;
  std::optional<Error> failure;
  std::promise<Result<std::vector<Out>>> promise;
  void finish(std::optional<Error> error = {}) {
    std::lock_guard lock(mutex);
    if (error && !failure)
      failure = std::move(error);
    --pending;
    publish();
  }
  void publish() {
    if (!sealed || pending || settled)
      return;
    settled = true;
    if (stop.cancelled() && !failure)
      failure = Error{ErrorCode::cancelled, "Parallel transform cancelled", {}};
    if (failure) {
      promise.set_value(std::unexpected(std::move(*failure)));
      return;
    }
    try {
      std::vector<Out> result;
      result.reserve(output.size());
      for (auto &value : output)
        result.push_back(std::move(value.value()));
      promise.set_value(std::move(result));
    } catch (...) {
      promise.set_value(fail(ErrorCode::failed,
                             "Parallel output construction failed",
                             std::current_exception()));
    }
  }
};
template <class State> struct ParallelTicket final {
  std::shared_ptr<State> state;
  bool done = false;
  explicit ParallelTicket(std::shared_ptr<State> state)
      : state(std::move(state)) {}
  ~ParallelTicket() {
    if (!done)
      state->finish(Error{ErrorCode::cancelled, "Parallel task discarded", {}});
  }
  void finish(std::optional<Error> error = {}) {
    done = true;
    state->finish(std::move(error));
  }
};
} // namespace detail
// Owned inputs, ordered outputs and cooperative cancellation between elements.
// A mapper copy belongs to each chunk; referenced/shared mapper state is the
// caller's synchronization responsibility. No std::execution backend required.
template <class In, class Map>
[[nodiscard]] auto parallel_transform(runtime::Runtime &runtime,
                                      std::vector<In> input, Map map,
                                      std::size_t grain = 64)
    -> ParallelResult<
        std::remove_cvref_t<std::invoke_result_t<Map, const In &>>> {
  using Out = std::remove_cvref_t<std::invoke_result_t<Map, const In &>>;
  using State = detail::ParallelState<In, Out>;
  static_assert(std::copy_constructible<Map> && !std::is_void_v<Out>);
  ParallelResult<Out> result;
  auto state = std::make_shared<State>();
  state->input = std::move(input);
  state->output.resize(state->input.size());
  state->stop = result.stop.token();
  result.completion = state->promise.get_future().share();
  if (!grain) {
    state->failure = Error{
        ErrorCode::invalid_argument, "Parallel grain must be positive", {}};
  } else {
    try {
      const auto count = state->input.size();
      const auto capacity = std::max(std::size_t{1}, runtime.stats().capacity);
      grain = std::max(grain, count / capacity + (count % capacity != 0));
      result.tokens.reserve(count / grain + (count % grain != 0));
      for (std::size_t begin = 0; begin < count;) {
        const auto end = begin + std::min(grain, count - begin);
        auto ticket = std::make_shared<detail::ParallelTicket<State>>(state);
        {
          std::lock_guard lock(state->mutex);
          ++state->pending;
        }
        runtime::Submission submission;
        try {
          submission =
              runtime.post([state, ticket, mapper = map, begin, end]() mutable {
                std::optional<Error> error;
                try {
                  for (auto i = begin; i < end; ++i) {
                    if (state->stop.cancelled()) {
                      error = Error{ErrorCode::cancelled,
                                    "Parallel transform cancelled",
                                    {}};
                      break;
                    }
                    state->output[i].emplace(
                        std::invoke(mapper, std::as_const(state->input[i])));
                  }
                } catch (...) {
                  error = Error{ErrorCode::failed, "Parallel mapper failed",
                                std::current_exception()};
                }
                ticket->finish(std::move(error));
              });
        } catch (...) {
          ticket->finish(Error{ErrorCode::failed, "Parallel submission failed",
                               std::current_exception()});
          throw;
        }
        if (!submission) {
          ticket->finish(Error{submission.error == runtime::SubmitError::full
                                   ? ErrorCode::full
                                   : ErrorCode::closed,
                               "Parallel admission rejected",
                               {}});
          result.cancel();
          break;
        }
        result.tokens.push_back(submission.token);
        begin = end;
      }
    } catch (...) {
      result.cancel();
      std::lock_guard lock(state->mutex);
      if (!state->failure)
        state->failure = Error{ErrorCode::failed, "Parallel submission failed",
                               std::current_exception()};
    }
  }
  {
    std::lock_guard lock(state->mutex);
    state->sealed = true;
    state->publish();
  }
  return result;
}
} // namespace ii::core
