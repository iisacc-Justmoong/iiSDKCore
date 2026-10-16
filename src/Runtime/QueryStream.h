#pragma once

#include "Query.h"
#include "Stream.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <vector>

namespace ii::runtime {

enum class QueryStreamErrorCode { stopped, invalid, exhausted, reentrant };
struct QueryStreamError final { QueryStreamErrorCode code; };
template<class T> using QueryStreamReply = std::expected<T, QueryStreamError>;
struct QueryStreamOptions final { std::size_t max_subscriptions = 16; };
struct QuerySubscriptionOptions final {
    std::size_t capacity = 64;
    OverflowPolicy overflow = OverflowPolicy::reject;
    std::size_t delivery_batch = 64;
    Priority priority = Priority::normal;
};
template<class T> struct QueryStreamEvent final {
    std::uint64_t sequence, revision;
    Clock::time_point produced;
    T value;
};
struct QuerySubscriptionStats final {
    std::size_t capacity = 0, pending = 0, high_watermark = 0;
    std::uint64_t revision = 1, matched = 0, filtered = 0, enqueued = 0,
                  delivered = 0, dropped = 0, rejected = 0, discarded = 0,
                  stale = 0, projection_failed = 0, callback_failed = 0,
                  dispatch_failed = 0;
    SubmitError last_dispatch = SubmitError::none;
    bool closed = false;
};
struct QueryStreamPublish final {
    std::uint64_t sequence = 0;
    std::size_t visited = 0, filtered = 0, enqueued = 0, rejected = 0,
                stale = 0, projection_failed = 0, dispatch_failed = 0;
    std::exception_ptr first_failure;
};

// A typed, bounded, in-process live feed. A null projection filters a source;
// a present Output owns exactly the fields selected by that subscriber's query.
// publish() borrows Source only for that call and serializes producer selection.
template<class Query, class Source, class Output>
class QueryStream final {
    static_assert(std::copy_constructible<Query>);
    static_assert(std::is_nothrow_move_constructible_v<Output> &&
                  std::is_nothrow_move_assignable_v<Output>);
public:
    using Event = QueryStreamEvent<Output>;
    using Projector = std::function<std::optional<Output>(const Query&, const Source&)>;
    using Handler = std::function<void(Event)>;
    using Revision = QueryStreamReply<std::uint64_t>;
private:
    using Post = std::function<Submission(Task, Priority)>;
    struct Dispatcher final {
        Post function;
        Submission post(Task task, Priority priority) { return function(std::move(task), priority); }
    };
    struct Subscriber final {
        std::mutex mutex;
        std::shared_ptr<const Query> selection;
        const QuerySubscriptionOptions options;
        // Indirection keeps arbitrary Output moves/destructors outside mutex.
        // The ring bounds live queued items; each projected item owns one node.
        std::vector<std::unique_ptr<Event>> slots;
        std::size_t head = 0, count = 0;
        std::uint64_t ticket = 0;
        bool scheduled = false;
        QuerySubscriptionStats statistics;
        Handler handler;
        Post dispatch;
        std::exception_ptr failure;
        std::atomic<bool> retired = false;
        TaskScope lifetime;
        QueryPort<Query, Revision> queries;

        Subscriber(Query query, Handler receiver, Post executor, QuerySubscriptionOptions bounds)
            : selection(std::make_shared<const Query>(std::move(query))), options(bounds),
              slots(bounds.capacity), handler(std::move(receiver)), dispatch(std::move(executor)),
              lifetime([this] { cleanup(); }),
              queries([this](const Query& query) { return replace(query); }) {
            statistics.capacity = bounds.capacity;
        }
        Revision replace(const Query& query) {
            auto next = std::make_shared<const Query>(query);
            std::vector<std::unique_ptr<Event>> previous(options.capacity);
            std::uint64_t revision;
            {
                std::lock_guard lock(mutex);
                if (statistics.closed) return std::unexpected(QueryStreamError{QueryStreamErrorCode::stopped});
                if (statistics.revision == std::numeric_limits<std::uint64_t>::max())
                    return std::unexpected(QueryStreamError{QueryStreamErrorCode::exhausted});
                selection.swap(next);
                slots.swap(previous);
                statistics.discarded += count;
                statistics.pending = count = head = 0;
                revision = ++statistics.revision;
            }
            return revision; // old query and payload destructors run without the state lock
        }
        ThreadResult close() {
            if (queries.is_current() || lifetime.is_current())
                return std::unexpected(ThreadError{ThreadErrorCode::self_join, {}});
            { std::lock_guard lock(mutex); statistics.closed = true; }
            if (auto result = queries.close(); !result) return result;
            return lifetime.close();
        }
        void cleanup() {
            {
                std::vector<std::unique_ptr<Event>> old_slots;
                std::shared_ptr<const Query> old_query;
                Handler old_handler;
                Post old_dispatch;
                std::exception_ptr old_failure;
                {
                    std::lock_guard lock(mutex);
                    old_slots.swap(slots); old_query.swap(selection);
                    old_handler.swap(handler); old_dispatch.swap(dispatch); std::swap(old_failure, failure);
                    statistics.discarded += count;
                    statistics.pending = count = head = 0;
                    statistics.closed = true;
                    scheduled = false;
                }
            }
            retired.store(true, std::memory_order_release);
        }
        QuerySubscriptionStats stats() {
            std::lock_guard lock(mutex);
            return statistics;
        }
    };
    // Executor cancellation destroys this ticket too, so a cancelled pump never
    // wedges admission. resume() or a later publish can retry the retained data.
    struct Ticket final {
        std::weak_ptr<Subscriber> owner;
        std::uint64_t generation;
        std::atomic<bool> released = false;
        Ticket(const std::shared_ptr<Subscriber>& state, std::uint64_t value)
            : owner(state), generation(value) {}
        ~Ticket() { release(); }
        void release() {
            if (released.exchange(true)) return;
            if (auto state = owner.lock()) {
                std::lock_guard lock(state->mutex);
                if (state->ticket == generation) state->scheduled = false;
            }
        }
    };
    static Submission schedule(const std::shared_ptr<Subscriber>& state) {
        Submission result{{}, SubmitError::stopped};
        (void)state->lifetime.run([&] {
            std::shared_ptr<Ticket> ticket;
            {
                std::lock_guard lock(state->mutex);
                if (state->statistics.closed) return;
                if (!state->count || state->scheduled) { result = {}; return; }
                ticket = std::make_shared<Ticket>(state, state->ticket + 1);
                ++state->ticket;
                state->scheduled = true;
            }
            // lifetime.run pins dispatch until this call returns; do not copy a
            // user endpoint under mutex (its copy constructor can reenter).
            Dispatcher executor{[&state](Task task, Priority priority) {
                return state->dispatch(std::move(task), priority);
            }};
            result = state->lifetime.post(executor, [weak=std::weak_ptr<Subscriber>(state), ticket] {
                if (auto target = weak.lock()) drain(target, ticket);
            }, state->options.priority);
            if (!result) {
                ticket->release();
            }
            {
                std::lock_guard lock(state->mutex);
                if (!result) ++state->statistics.dispatch_failed;
                state->statistics.last_dispatch = result.error;
            }
        });
        return result;
    }
    static void drain(const std::shared_ptr<Subscriber>& state, const std::shared_ptr<Ticket>& ticket) {
        for (std::size_t i = 0; i < state->options.delivery_batch; ++i) {
            std::unique_ptr<Event> next;
            {
                std::lock_guard lock(state->mutex);
                if (state->statistics.closed || !state->count) break;
                next.swap(state->slots[state->head]);
                state->head = (state->head + 1) % state->slots.size();
                state->statistics.pending = --state->count;
            }
            try { state->handler(std::move(*next)); }
            catch (...) {
                auto failure = std::current_exception();
                {
                    std::lock_guard lock(state->mutex);
                    ++state->statistics.callback_failed;
                    std::swap(state->failure, failure);
                }
                throw; // preserve the executor's normal callback failure contract
            }
            { std::lock_guard lock(state->mutex); ++state->statistics.delivered; }
        }
        ticket->release();
        (void)schedule(state); // bounded batch; saturation is observable and retryable
    }

public:
    // Move-only ownership; removal waits for selection, delivery and captures.
    // As with TaskScope, destruction from one's own callback is an error.
    class Subscription final {
        friend class QueryStream;
        std::shared_ptr<Subscriber> state_;
        explicit Subscription(std::shared_ptr<Subscriber> state) : state_(std::move(state)) {}
    public:
        Subscription(Subscription&& other) noexcept = default;
        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                if (!close()) std::terminate();
                state_ = std::move(other.state_);
            }
            return *this;
        }
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        ~Subscription() { if (!close()) std::terminate(); }
        [[nodiscard]] ThreadResult close() const { return state_ ? state_->close() : ThreadResult{}; }
        [[nodiscard]] QuerySubscriptionStats stats() const {
            return state_ ? state_->stats() : QuerySubscriptionStats{.closed=true};
        }
        [[nodiscard]] Submission resume() const {
            return state_ ? schedule(state_) : Submission{{}, SubmitError::stopped};
        }
        [[nodiscard]] std::exception_ptr failure() const {
            if (!state_) return {};
            std::lock_guard lock(state_->mutex);
            return state_->failure;
        }
        // The port itself remains valid until this handle is removed. A moved
        // handle has no port. QueryPort reports closed admission after close().
        [[nodiscard]] const QueryPort<Query, Revision>& queries() const {
            if (!state_) throw std::logic_error("moved QueryStream subscription has no query port");
            return state_->queries;
        }
    };

    explicit QueryStream(Projector selector, QueryStreamOptions options = {})
        : projector_(std::move(selector)),
          lifetime_([this] {
              Projector retired;
              { std::lock_guard lock(mutex_); retired.swap(projector_); }
          }) {
        if (!projector_ || !options.max_subscriptions || options.max_subscriptions > 4096)
            throw std::invalid_argument("invalid QueryStream projector or subscriber bounds");
        subscribers_.resize(options.max_subscriptions);
    }
    ~QueryStream() { if (!close()) std::terminate(); }
    QueryStream(const QueryStream&) = delete;
    QueryStream& operator=(const QueryStream&) = delete;

    template<TaskExecutor Endpoint> requires std::copy_constructible<Endpoint>
    [[nodiscard]] QueryStreamReply<Subscription> subscribe_on(
        Endpoint endpoint, Query query, Handler handler, QuerySubscriptionOptions options = {}) {
        return subscribe_impl(std::move(query), std::move(handler),
            [endpoint=std::move(endpoint)](Task task, Priority priority) mutable {
                return endpoint.post(std::move(task), priority);
            }, options);
    }
    // Borrowed executor: it must outlive close(). EventLoop's overload instead
    // stores its weak Sender, so removing that loop is safe.
    template<TaskExecutor Executor>
    [[nodiscard]] QueryStreamReply<Subscription> subscribe(
        Executor& executor, Query query, Handler handler, QuerySubscriptionOptions options = {}) {
        return subscribe_impl(std::move(query), std::move(handler),
            [&executor](Task task, Priority priority) { return executor.post(std::move(task), priority); }, options);
    }
    [[nodiscard]] QueryStreamReply<Subscription> subscribe(
        EventLoop& executor, Query query, Handler handler, QuerySubscriptionOptions options = {}) {
        return subscribe_on(executor.sender(), std::move(query), std::move(handler), options);
    }
    [[nodiscard]] QueryStreamReply<QueryStreamPublish> publish(const Source& source) {
        if (lifetime_.is_current()) return std::unexpected(QueryStreamError{QueryStreamErrorCode::reentrant});
        QueryStreamReply<QueryStreamPublish> result = std::unexpected(QueryStreamError{QueryStreamErrorCode::stopped});
        (void)lifetime_.run([&] {
            std::lock_guard producer(producer_);
            std::vector<std::shared_ptr<Subscriber>> targets;
            QueryStreamPublish report;
            {
                std::lock_guard lock(mutex_);
                if (sequence_ == std::numeric_limits<std::uint64_t>::max()) {
                    result = std::unexpected(QueryStreamError{QueryStreamErrorCode::exhausted}); return;
                }
                report.sequence = ++sequence_;
                targets = subscribers_;
            }
            const auto produced = Clock::now();
            for (const auto& target : targets) {
                if (!target) continue;
                (void)target->lifetime.run([&] {
                    std::shared_ptr<const Query> selection;
                    std::uint64_t revision;
                    {
                        std::lock_guard lock(target->mutex);
                        if (target->statistics.closed) return;
                        selection = target->selection;
                        revision = target->statistics.revision;
                    }
                    ++report.visited;
                    std::optional<Output> projected;
                    try { projected = projector_(*selection, source); }
                    catch (...) {
                        auto failure = std::current_exception();
                        if (!report.first_failure) report.first_failure = failure;
                        ++report.projection_failed;
                        {
                            std::lock_guard lock(target->mutex);
                            ++target->statistics.projection_failed;
                            std::swap(target->failure, failure);
                        }
                        return;
                    }
                    std::unique_ptr<Event> packet;
                    if (projected)
                        packet = std::make_unique<Event>(Event{report.sequence, revision, produced, std::move(*projected)});
                    std::unique_ptr<Event> retired;
                    {
                        std::lock_guard lock(target->mutex);
                        if (target->statistics.closed || revision != target->statistics.revision) {
                            ++target->statistics.stale; ++report.stale; return;
                        }
                        if (!packet) {
                            ++target->statistics.filtered; ++report.filtered; return;
                        }
                        ++target->statistics.matched;
                        if (target->count == target->slots.size()) {
                            if (target->options.overflow == OverflowPolicy::reject) {
                                ++target->statistics.rejected; ++report.rejected; return;
                            }
                            retired.swap(target->slots[target->head]);
                            target->head = (target->head + 1) % target->slots.size();
                            --target->count; ++target->statistics.dropped;
                        }
                        target->slots[(target->head + target->count) % target->slots.size()].swap(packet);
                        target->statistics.pending = ++target->count;
                        ++target->statistics.enqueued; ++report.enqueued;
                        target->statistics.high_watermark = std::max(target->count, target->statistics.high_watermark);
                    }
                    if (!schedule(target)) ++report.dispatch_failed;
                });
            }
            result = std::move(report);
        });
        return result;
    }
    [[nodiscard]] ThreadResult close() {
        if (lifetime_.is_current()) return std::unexpected(ThreadError{ThreadErrorCode::self_join, {}});
        std::vector<std::shared_ptr<Subscriber>> targets;
        { std::lock_guard lock(mutex_); targets = subscribers_; }
        for (const auto& target : targets)
            if (target && (target->lifetime.is_current() || target->queries.is_current()))
                return std::unexpected(ThreadError{ThreadErrorCode::self_join, {}});
        if (auto result = lifetime_.close(); !result) return result;
        // Re-snapshot after the admission barrier includes concurrently accepted subscriptions.
        { std::lock_guard lock(mutex_); targets = subscribers_; }
        for (const auto& target : targets)
            if (target) if (auto result = target->close(); !result) return result;
        { std::lock_guard lock(mutex_); subscribers_.clear(); }
        return {};
    }
private:
    QueryStreamReply<Subscription> subscribe_impl(Query query, Handler handler, Post dispatch,
                                                 QuerySubscriptionOptions options) {
        if (!handler || !options.capacity || options.capacity > 65536 || !options.delivery_batch ||
            options.delivery_batch > 65536 ||
            (options.overflow != OverflowPolicy::reject && options.overflow != OverflowPolicy::drop_oldest) ||
            (options.priority != Priority::high && options.priority != Priority::normal && options.priority != Priority::background))
            return std::unexpected(QueryStreamError{QueryStreamErrorCode::invalid});
        QueryStreamReply<Subscription> result = std::unexpected(QueryStreamError{QueryStreamErrorCode::stopped});
        (void)lifetime_.run([&] {
            auto state = std::make_shared<Subscriber>(std::move(query), std::move(handler), std::move(dispatch), options);
            std::shared_ptr<Subscriber> retired;
            {
                std::lock_guard lock(mutex_);
                const auto slot = std::find_if(subscribers_.begin(), subscribers_.end(), [](const auto& value) {
                    return !value || value->retired.load(std::memory_order_acquire);
                });
                if (slot == subscribers_.end()) {
                    result = std::unexpected(QueryStreamError{QueryStreamErrorCode::exhausted}); return;
                }
                retired.swap(*slot);
                *slot = state;
            }
            result = Subscription(std::move(state));
        });
        return result;
    }
    std::mutex mutex_, producer_;
    std::vector<std::shared_ptr<Subscriber>> subscribers_;
    Projector projector_;
    std::uint64_t sequence_ = 0;
    TaskScope lifetime_;
};

} // namespace ii::runtime
