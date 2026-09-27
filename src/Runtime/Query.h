#pragma once

#include "EventLoop.h"
#include "Runtime.h"
#include <concepts>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ii::runtime {

enum class QueryErrorCode { receiver_expired, handler_failed, wrong_executor };
struct QueryError {
    QueryErrorCode code;
    std::exception_ptr cause;
};
template<class Response>
using QueryReply = std::expected<Response, QueryError>;

// Objects own ports and communicate through port references. Queries include
// commands and notifications; they are not limited to read-only requests.
// Request/Response describe only needed values. Field selection belongs to the
// typed contract; sender/receiver objects are never implicitly added to payloads.
template<class Request, class Response = void>
class QueryPort final {
public:
    using Handler = std::function<Response(const Request&)>;
    using Completion = std::function<void(QueryReply<Response>)>;

    explicit QueryPort(Handler handler) {
        if (!handler) throw std::invalid_argument("QueryPort requires a handler");
        state_ = std::make_shared<State>(std::move(handler));
    }
    QueryPort(SerialQueue& executor, Handler handler) : QueryPort(std::move(handler)) {
        executor_ = std::ref(executor);
    }
    QueryPort(const QueryPort&) = delete;
    QueryPort& operator=(const QueryPort&) = delete;
    QueryPort(QueryPort&&) = delete;
    QueryPort& operator=(QueryPort&&) = delete;

    // Process on the caller's thread, borrowing the request for this call.
    [[nodiscard]] QueryReply<Response> send(const Request& request) const {
        if (executor_ && !executor_->get().is_current())
            return std::unexpected(QueryError{QueryErrorCode::wrong_executor, {}});
        const auto receiver = state_;
        return invoke(*receiver, request);
    }

    // Admission is distinct from delivery. EventLoop cancellation and shutdown
    // can suppress both the handler and completion callback.
    template<class Executor>
    [[nodiscard]] Submission post(Executor& loop, Request request, Completion completion,
                                  Priority priority = Priority::normal) const
        requires std::copy_constructible<Request> && requires(Executor& target, Task task) {
            { target.post(std::move(task), Priority::normal) } -> std::same_as<Submission>;
        } {
        if (executor_) {
            if constexpr (std::same_as<std::remove_cv_t<Executor>, SerialQueue>) {
                if (&loop != &executor_->get()) return {{}, SubmitError::invalid};
            } else return {{}, SubmitError::invalid};
        }
        if (!completion) return {{}, SubmitError::invalid};
        return loop.post(
            [receiver = std::weak_ptr<State>(state_), request = std::move(request),
             completion = std::move(completion)]() mutable {
                if (const auto target = receiver.lock()) completion(invoke(*target, request));
                else completion(std::unexpected(QueryError{QueryErrorCode::receiver_expired, {}}));
            }, priority);
    }

    // Bound object ports use asynchronous delivery without choosing a thread or
    // executor for each call. Completion executes in the same serial domain.
    [[nodiscard]] Submission post(Request request, Completion completion,
                                  Priority priority = Priority::normal) const
        requires std::copy_constructible<Request> {
        if (!executor_) return {{}, SubmitError::invalid};
        return post(executor_->get(), std::move(request), std::move(completion), priority);
    }

private:
    struct State { Handler handler; };
    std::shared_ptr<State> state_;
    std::optional<std::reference_wrapper<SerialQueue>> executor_;

    static QueryReply<Response> invoke(const State& receiver, const Request& request) {
        try {
            if constexpr (std::is_void_v<Response>) {
                receiver.handler(request);
                return {};
            } else return receiver.handler(request);
        } catch (...) {
            return std::unexpected(QueryError{QueryErrorCode::handler_failed, std::current_exception()});
        }
    }
};

} // namespace ii::runtime
