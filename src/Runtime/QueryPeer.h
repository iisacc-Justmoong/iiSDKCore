#pragma once

#include "Query.h"
#include <atomic>
#include <expected>
#include <memory>
#include <mutex>
#include <utility>

namespace ii::runtime {

enum class QueryPeerConnectError { same_node, busy, closed };

// An endpoint owns its receiver and holds only a weak route to its peer. An
// object can own several endpoints when it needs several independent links.
template<class Incoming, class IncomingReply = void,
         class Outgoing = Incoming, class OutgoingReply = IncomingReply>
class QueryPeer final {
    template<class, class, class, class> friend class QueryPeer;
    using Receiver = QueryPort<Incoming, IncomingReply>;
    using Remote = QueryPort<Outgoing, OutgoingReply>;
    using Opposite = QueryPeer<Outgoing, OutgoingReply, Incoming, IncomingReply>;

    std::shared_ptr<Receiver> receiver_;
    mutable std::mutex mutex_;
    std::weak_ptr<Remote> remote_;
    std::shared_ptr<std::atomic<bool>> linked_;
    bool closed_ = false;

    [[nodiscard]] bool linked_locked() const noexcept {
        return linked_ && linked_->load(std::memory_order_acquire) && !remote_.expired();
    }
    void disconnect_locked() noexcept {
        if (linked_) linked_->store(false, std::memory_order_release);
        linked_.reset();
        remote_.reset();
    }
    [[nodiscard]] std::shared_ptr<Remote> remote() const {
        std::lock_guard lock(mutex_);
        if (closed_ || !linked_locked()) return {};
        return remote_.lock();
    }
    void stop_admission() noexcept {
        std::lock_guard lock(mutex_);
        closed_ = true;
        disconnect_locked();
    }

public:
    using Handler = typename Receiver::Handler;
    using Completion = typename Remote::Completion;

    explicit QueryPeer(Handler handler)
        : receiver_(std::make_shared<Receiver>(std::move(handler))) {}
    QueryPeer(SerialQueue& executor, Handler handler)
        : receiver_(std::make_shared<Receiver>(executor, std::move(handler))) {}
    QueryPeer(const QueryPeer&) = delete;
    QueryPeer& operator=(const QueryPeer&) = delete;
    QueryPeer(QueryPeer&&) = delete;
    QueryPeer& operator=(QueryPeer&&) = delete;
    ~QueryPeer() {
        auto result = close();
        if (!result && result.error().code != ThreadErrorCode::work_failed) std::terminate();
    }

    [[nodiscard]] std::expected<void, QueryPeerConnectError> connect(Opposite& other) {
        if (static_cast<const void*>(this) == static_cast<const void*>(std::addressof(other)))
            return std::unexpected(QueryPeerConnectError::same_node);
        std::scoped_lock lock(mutex_, other.mutex_);
        if (closed_ || other.closed_) return std::unexpected(QueryPeerConnectError::closed);
        if (linked_locked() || other.linked_locked()) return std::unexpected(QueryPeerConnectError::busy);
        auto link = std::make_shared<std::atomic<bool>>(true);
        remote_ = other.receiver_;
        other.remote_ = receiver_;
        linked_ = link;
        other.linked_ = std::move(link);
        return {};
    }
    void disconnect() noexcept {
        std::lock_guard lock(mutex_);
        disconnect_locked();
    }
    [[nodiscard]] bool connected() const {
        std::lock_guard lock(mutex_);
        return !closed_ && linked_locked();
    }

    [[nodiscard]] QueryReply<OutgoingReply> send(const Outgoing& request) const {
        if (auto target = remote()) return target->send(request);
        return std::unexpected(QueryError{QueryErrorCode::receiver_expired, {}});
    }
    template<class Executor>
    [[nodiscard]] Submission post(Executor& executor, Outgoing request, Completion completion,
                                  Priority priority = Priority::normal) const
        requires std::copy_constructible<Outgoing> && TaskExecutor<Executor> {
        if (auto target = remote())
            return target->post(executor, std::move(request), std::move(completion), priority);
        return {{}, SubmitError::stopped};
    }
    [[nodiscard]] Submission post(Outgoing request, Completion completion,
                                  Priority priority = Priority::normal) const
        requires std::copy_constructible<Outgoing> {
        if (auto target = remote())
            return target->post(std::move(request), std::move(completion), priority);
        return {{}, SubmitError::stopped};
    }

    [[nodiscard]] ThreadResult close() {
        if (receiver_->is_current()) return receiver_->close();
        stop_admission();
        return receiver_->close();
    }
    [[nodiscard]] ThreadResult try_close() {
        if (receiver_->is_current()) return receiver_->try_close();
        stop_admission();
        return receiver_->try_close();
    }
};

} // namespace ii::runtime
