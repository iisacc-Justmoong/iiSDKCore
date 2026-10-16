#include <iiSDKCore/Runtime/QueryStream.h>
#include <iiSDKCore/Runtime/HostLoop.h>
#include <atomic>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <string>
#include <thread>

using namespace ii::runtime;
using namespace std::chrono_literals;
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct Sample final { int device = 0, level = 0; std::string label; };
struct Select final { int device = 0; bool level = false, label = false; };
struct Fields final { std::optional<int> level; std::optional<std::string> label; };
using Feed = QueryStream<Select, Sample, Fields>;
using Event = QueryStreamEvent<Fields>;

void selection_and_revision() {
    EventLoop loop;
    int label_reads = 0;
    Feed feed([&](const Select& q, const Sample& source) -> std::optional<Fields> {
        if (q.device != source.device) return {};
        Fields result;
        if (q.level) result.level = source.level;
        if (q.label) { ++label_reads; result.label = source.label; }
        return result;
    });
    std::vector<Event> a, b;
    auto first = feed.subscribe_on(loop.sender(), {1, true, false}, [&](Event event) { a.push_back(std::move(event)); });
    auto second = feed.subscribe_on(loop.sender(), {2, false, true}, [&](Event event) { b.push_back(std::move(event)); });
    check(first.has_value() && second.has_value(), "independent subscriptions");
    auto one = feed.publish({1, 0, "private"});
    auto two = feed.publish({2, 8, "selected"});
    check(one && one->enqueued == 1 && one->filtered == 1 && two && two->enqueued == 1,
          "source filtering is per query");
    check(a.empty() && b.empty() && label_reads == 1, "delivery deferred; unselected label never evaluated");
    loop.poll({16, 1s});
    check(a.size() == 1 && a[0].value.level == 0 && !a[0].value.label,
          "selected zero is distinct from an omitted field");
    check(b.size() == 1 && b[0].value.label == "selected" && !b[0].value.level,
          "selected fields only");
    check(a[0].sequence == 1 && b[0].sequence == 2 && a[0].revision == 1,
          "source sequence and subscription revision");
    check(bool(feed.publish({1, 99, "stale"})), "old query queued");
    const auto changed = first->queries().send({1, false, true});
    check(changed && *changed && **changed == 2, "typed query update increments revision");
    check(bool(feed.publish({1, 55, "new"})), "new query queued");
    loop.poll({16, 1s});
    check(a.size() == 2 && a[1].revision == 2 && a[1].value.label == "new" && !a[1].value.level,
          "query replacement discards queued old data");
    check(first->stats().discarded == 1, "query discard is observable");
    check(bool(first->close()) && bool(second->close()) && bool(feed.close()), "normal removal");
    check(!feed.publish({}), "closed producer rejects source");
}

void bounds_and_retry() {
    EventLoop loop(2);
    QueryStream<int, int, int> feed([](int, int value) { return std::optional<int>(value); }, {1});
    std::vector<int> received;
    auto sub = feed.subscribe_on(loop.sender(), 0, [&](auto event) { received.push_back(event.value); },
                                 {.capacity=2, .overflow=OverflowPolicy::reject});
    check(sub.has_value(), "bounded subscription");
    check(!feed.subscribe_on(loop.sender(), 0, [](auto) {}), "subscriber quota");
    check(feed.publish(1)->enqueued == 1 && feed.publish(2)->enqueued == 1 && feed.publish(3)->rejected == 1,
          "reject policy preserves admitted data");
    loop.poll({16, 1s});
    check(received == std::vector<int>({1, 2}) && sub->stats().rejected == 1, "bounded queue delivered FIFO");
    check(bool(sub->close()), "quota released after cleanup");
    received.clear();
    auto replacement = feed.subscribe_on(loop.sender(), 0, [&](auto event) { received.push_back(event.value); },
                                         {.capacity=2, .overflow=OverflowPolicy::drop_oldest});
    check(replacement.has_value(), "closed slot reused");
    const auto old_query = sub->queries().send(100);
    check(!old_query && old_query.error().code == QueryErrorCode::receiver_expired,
          "a retired handle cannot control a replacement in the reused slot");
    (void)feed.publish(4); (void)feed.publish(5); (void)feed.publish(6);
    loop.poll({16, 1s});
    check(received == std::vector<int>({5, 6}) && replacement->stats().dropped == 1,
          "drop-oldest policy keeps latest admitted data");
    (void)loop.post([] {}); (void)loop.post([] {});
    auto blocked = feed.publish(7);
    check(blocked && blocked->dispatch_failed == 1 && replacement->stats().pending == 1,
          "executor backpressure keeps data queued");
    loop.poll({16, 1s});
    check(bool(replacement->resume()), "explicit retry after executor capacity returns");
    loop.poll({16, 1s});
    check(received.back() == 7, "retry emits original queued data");
}

void failures_and_lifetimes() {
    EventLoop loop;
    using Integers = QueryStream<int, int, int>;
    Integers feed([](int query, int value) -> std::optional<int> {
        if (query < 0) throw std::runtime_error("selection failed");
        return value;
    });
    int delivered = 0;
    auto bad = feed.subscribe(loop, -1, [](auto) {});
    auto good = feed.subscribe(loop, 1, [&](auto) { ++delivered; });
    const auto result = feed.publish(1);
    check(result && result->projection_failed == 1 && result->enqueued == 1,
          "one projection failure does not abort independent subscriptions");
    bool original = false;
    try { std::rethrow_exception(result->first_failure); }
    catch (const std::runtime_error& error) { original = std::string(error.what()) == "selection failed"; }
    check(original && bad->failure(), "original projection cause preserved");
    loop.poll({16, 1s});
    check(delivered == 1, "healthy subscriber continues");
    bool should_throw = true;
    auto throwing = feed.subscribe(loop, 0, [&](auto) {
        if (should_throw) { should_throw = false; throw std::runtime_error("delivery failed"); }
        ++delivered;
    });
    (void)feed.publish(2); (void)feed.publish(3);
    bool caught = false;
    try { loop.poll({32, 1s}); }
    catch (const std::runtime_error&) { caught = true; }
    check(caught && throwing->stats().callback_failed == 1 && throwing->failure(),
          "delivery failure preserves executor failure semantics");
    check(bool(throwing->resume()), "callback failure permits explicit retry of remaining data");
    loop.poll({32, 1s});
    check(throwing->stats().delivered == 1 && throwing->stats().pending == 0, "remaining delivery recovered");

    std::weak_ptr<int> receiver;
    std::optional<Integers::Subscription> surviving;
    {
        Integers temporary([](int, int value) { return std::optional<int>(value); });
        auto resource = std::make_shared<int>(7);
        receiver = resource;
        auto subscription = temporary.subscribe(loop, 0, [resource](auto) {});
        surviving.emplace(std::move(*subscription));
        (void)temporary.publish(4);
    }
    check(receiver.expired() && surviving->stats().closed && loop.stats().pending == 0,
          "producer removal cancels pending pumps and releases receiver captures");
    const auto expired = surviving->queries().send(5);
    check(!expired && expired.error().code == QueryErrorCode::receiver_expired,
          "surviving handle has an expired port after producer removal");
    check(surviving->resume().error == SubmitError::stopped, "surviving handle rejects delivery");

    std::optional<Integers::Subscription> no_loop;
    {
        EventLoop temporary;
        auto subscription = feed.subscribe(temporary, 0, [](auto) {});
        no_loop.emplace(std::move(*subscription));
        (void)feed.publish(8);
    }
    check(no_loop->resume().error == SubmitError::stopped && bool(no_loop->close()),
          "weak EventLoop endpoint remains safe after executor destruction");
}

void query_port_and_batching() {
    EventLoop loop(8);
    QueryStream<int, int, int> feed([](int query, int source) { return std::optional<int>(query * source); });
    std::vector<int> data;
    auto sub = feed.subscribe(loop, 1, [&](auto event) { data.push_back(event.value); }, {.capacity=8, .delivery_batch=1});
    (void)feed.publish(1); (void)feed.publish(2); (void)feed.publish(3);
    bool ordinary = false;
    (void)loop.post([&] { ordinary = data.size() == 1; });
    loop.poll({32, 1s});
    check(ordinary && data == std::vector<int>({1, 2, 3}), "bounded batches allow other runtime work to run");
    bool query_done = false;
    int query = 10;
    auto update = sub->queries().post(loop, query, [&](auto reply) { query_done = reply && *reply && **reply == 2; });
    query = 99;
    check(bool(update), "typed query port accepts asynchronous query replacement");
    loop.poll({16, 1s});
    (void)feed.publish(4);
    loop.poll({16, 1s});
    check(query_done && data.back() == 40, "query owns original selection across deferred control delivery");

    // Executor-side cancellation releases the scheduling ticket, including when
    // cancellation was not requested by the stream itself.
    loop.suspend();
    (void)feed.publish(5);
    check(bool(sub->close()) && loop.stats().pending == 0, "close cancels pump even while host suspended");
    loop.resume();

    using MoveFeed = QueryStream<int, int, std::unique_ptr<int>>;
    MoveFeed move([](int, int source) { return std::make_optional(std::make_unique<int>(source)); });
    int moved = 0;
    auto owner = move.subscribe(loop, 0, [&](auto event) { moved = *event.value; });
    (void)move.publish(12); loop.poll({16, 1s});
    check(moved == 12, "stream output can transfer move-only owned data");
}

void concurrent_query_replacement() {
    EventLoop loop;
    std::latch entered(1), release(1);
    QueryStream<int, int, int> feed([&](int query, int source) {
        if (query == 1) { entered.count_down(); release.wait(); }
        return std::optional<int>(query * source);
    });
    std::vector<int> data;
    auto sub = feed.subscribe(loop, 1, [&](auto event) { data.push_back(event.value); });
    QueryStreamReply<QueryStreamPublish> result = std::unexpected(QueryStreamError{QueryStreamErrorCode::invalid});
    std::thread producer([&] { result = feed.publish(4); });
    entered.wait();
    const auto updated = sub->queries().send(2);
    release.count_down(); producer.join();
    check(updated && *updated && result && result->stale == 1 && result->enqueued == 0,
          "in-flight old projection cannot enter the new query revision");
    (void)feed.publish(5); loop.poll({16, 1s});
    check(data == std::vector<int>({10}), "only new query output emitted after racing replacement");
}

void workers_and_removal() {
    Runtime runtime({.workers=3, .capacity=32});
    SerialQueue receiver(runtime);
    QueryStream<int, int, int> feed([](int, int source) { return std::optional<int>(source); });
    std::vector<std::uint64_t> order;
    std::atomic<int> active = 0;
    std::atomic<bool> serialized = true;
    auto sub = feed.subscribe(receiver, 0, [&](auto event) {
        if (++active != 1 || !receiver.is_current()) serialized = false;
        order.push_back(event.sequence);
        --active;
    }, {.capacity=2048});
    std::atomic<std::size_t> admitted = 0;
    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) producers.emplace_back([&] {
        for (int i = 0; i < 200; ++i) {
            const auto result = feed.publish(i);
            if (result) admitted += result->enqueued;
        }
    });
    for (auto& producer : producers) producer.join();
    check(bool(runtime.wait_idle()), "runtime drains repeated delivery batches");
    if (sub->stats().pending) { check(bool(sub->resume()), "retry worker contention"); check(bool(runtime.wait_idle()), "drain retry"); }
    check(admitted == 800 && order.size() == 800 && serialized && std::is_sorted(order.begin(), order.end()),
          "multiple producers and persistent workers preserve per-subscriber order without overlap");
    check(bool(sub->close()), "worker subscription removal");

    std::latch entered(1), release(1);
    std::atomic<bool> finished = false;
    std::weak_ptr<int> resource;
    auto capture = std::make_shared<int>(1); resource = capture;
    auto blocking = feed.subscribe(receiver, 0, [&, capture](auto) {
        const auto own = feed.close();
        check(!own && own.error().code == ThreadErrorCode::self_join, "producer self-close rejected");
        entered.count_down(); release.wait();
    });
    capture.reset();
    (void)feed.publish(1); entered.wait();
    std::thread closer([&] { check(bool(blocking->close()), "external removal barrier"); finished = true; });
    while (!blocking->stats().closed) std::this_thread::yield();
    const bool waited = !finished.load() && !resource.expired();
    release.count_down(); closer.join();
    check(waited && finished && resource.expired(), "removal waits for active callback before destroying captures");
    check(bool(runtime.wait_idle()), "worker captures retired");
}

void cancellation_and_host() {
    struct Endpoint final {
        EventLoop::Sender sender;
        std::shared_ptr<TaskToken> token;
        Submission post(Task task, Priority priority) {
            auto result = sender.post(std::move(task), priority);
            *token = result.token;
            return result;
        }
    };
    EventLoop loop;
    auto token = std::make_shared<TaskToken>();
    QueryStream<int, int, int> feed([](int, int source) { return std::optional<int>(source); });
    int emitted = 0;
    const auto owner = std::this_thread::get_id();
    auto sub = feed.subscribe_on(Endpoint{loop.sender(), token}, 0, [&](auto event) {
        check(owner == std::this_thread::get_id(), "host receives output on its native owner thread");
        emitted = event.value;
    });
    (void)feed.publish(1);
    check(token->cancel() && sub->stats().pending == 1, "executor cancellation retains bounded output");
    check(bool(sub->resume()), "cancelled scheduling ticket can be retried");
    loop.poll({16, 1s});
    check(emitted == 1, "cancel/retry emits once");

    std::atomic<int> wakes = 0;
    HostLoop host(loop, [&]() noexcept { ++wakes; }, {.monitor_io=false});
    std::thread producer([&] { (void)feed.publish(2); });
    producer.join();
    check(wakes > 0 && emitted == 1, "background stream wakes host without dispatching callbacks");
    loop.poll({16, 1s});
    check(emitted == 2 && bool(sub->close()) && bool(host.close()), "native host dispatch and detach");
}

void quota_after_capture_destruction() {
    EventLoop loop;
    QueryStream<int, int, int> feed([](int, int source) { return std::optional<int>(source); }, {1});
    std::latch destroying(1), release(1);
    auto capture = std::shared_ptr<int>(new int(1), [&](int* pointer) {
        destroying.count_down(); release.wait(); delete pointer;
    });
    auto sub = feed.subscribe(loop, 0, [capture](auto) {});
    capture.reset();
    std::thread closer([&] { check(bool(sub->close()), "capture cleanup barrier"); });
    destroying.wait();
    const auto early = feed.subscribe(loop, 0, [](auto) {});
    const bool protected_quota = !early && early.error().code == QueryStreamErrorCode::exhausted;
    release.count_down(); closer.join();
    check(protected_quota, "quota is not returned while receiver captures are being destroyed");
    auto next = feed.subscribe(loop, 0, [](auto) {});
    check(next.has_value(), "quota becomes available after capture destruction completes");
}

void parallel_executor_and_worker_close() {
    Runtime runtime({.workers=3, .capacity=32});
    QueryStream<int, int, int> feed([](int, int source) { return std::optional<int>(source); });
    std::atomic<int> active = 0;
    std::atomic<bool> serialized = true;
    int count = 0;
    auto sub = feed.subscribe(runtime, 0, [&](auto) {
        if (++active != 1) serialized = false;
        ++count;
        std::this_thread::yield();
        --active;
    }, {.capacity=512, .delivery_batch=1});
    for (int i = 0; i < 200; ++i) (void)feed.publish(i);
    check(bool(runtime.wait_idle()), "parallel executor drains stream");
    check(serialized && count == 200 && bool(sub->close()), "stream serializes its own receiver on a parallel executor");

    std::latch entered(1), release(1), returned(1);
    auto blocking = feed.subscribe(runtime, 0, [&](auto) { entered.count_down(); release.wait(); });
    (void)feed.publish(1); entered.wait();
    std::atomic<bool> no_block = false;
    check(bool(runtime.post([&] {
        const auto result = blocking->close();
        no_block = !result && result.error().code == ThreadErrorCode::would_block;
        returned.count_down();
    })), "worker close probe submitted");
    returned.wait(); release.count_down();
    check(no_block && bool(blocking->close()) && bool(runtime.wait_idle()),
          "worker cannot block on active callback; external retry completes removal");
}

void validation_and_reentry() {
    EventLoop loop;
    using Integers = QueryStream<int, int, int>;
    Integers* self = nullptr;
    bool guarded = false;
    Integers feed([&](int, int source) {
        const auto nested = self->publish(0);
        const auto close = self->close();
        guarded = !nested && nested.error().code == QueryStreamErrorCode::reentrant &&
                  !close && close.error().code == ThreadErrorCode::self_join;
        return std::optional<int>(source);
    });
    self = &feed;
    check(!feed.subscribe(loop, 0, {}, {}), "missing subscriber callback rejected");
    check(!feed.subscribe(loop, 0, [](auto) {}, {.capacity=0}), "zero queue bound rejected");
    check(!feed.subscribe(loop, 0, [](auto) {}, {.capacity=1, .delivery_batch=0}), "zero delivery batch rejected");
    auto sub = feed.subscribe(loop, 0, [](auto) {});
    (void)feed.publish(1);
    check(guarded, "recursive selection cannot deadlock or close its own source");
    loop.poll({16, 1s});
    check(bool(sub->close()) && bool(sub->close()), "subscription removal is idempotent");
    bool invalid = false;
    try { Integers broken({}, {}); } catch (const std::invalid_argument&) { invalid = true; }
    check(invalid, "invalid stream construction rejected");
}

void payload_reentry() {
    struct Payload final {
        int value;
        std::shared_ptr<std::function<void()>> observe;
        Payload(int v, std::shared_ptr<std::function<void()>> hook) : value(v), observe(std::move(hook)) {}
        Payload(Payload&& other) noexcept : value(other.value), observe(other.observe) { (*observe)(); }
        Payload& operator=(Payload&& other) noexcept {
            value = other.value; observe = other.observe; (*observe)(); return *this;
        }
        ~Payload() { if (observe) (*observe)(); }
    };
    using ReentrantFeed = QueryStream<int, int, Payload>;
    EventLoop loop;
    const ReentrantFeed::Subscription* receiver = nullptr;
    int observations = 0;
    auto hook = std::make_shared<std::function<void()>>([&] {
        if (receiver) { (void)receiver->stats(); ++observations; }
    });
    ReentrantFeed feed([&](int, int value) { return std::optional<Payload>(std::in_place, value, hook); });
    int delivered = 0;
    auto sub = feed.subscribe(loop, 0, [&](auto event) { delivered += event.value.value; },
                              {.capacity=1, .overflow=OverflowPolicy::drop_oldest});
    receiver = &*sub;
    (void)feed.publish(1); (void)feed.publish(2);
    loop.poll({16, 1s});
    (void)feed.publish(3);
    check(bool(sub->queries().send(1)), "query replacement with reentrant payload destructors");
    (void)feed.publish(4);
    check(bool(sub->close()) && delivered == 2 && observations > 0,
          "payload move/destruction can inspect stream state without a lock deadlock");
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--payload-reentry") {
            payload_reentry();
            std::cout << "payload reentry passed\n";
            return 0;
        }
        selection_and_revision();
        bounds_and_retry();
        failures_and_lifetimes();
        query_port_and_batching();
        concurrent_query_replacement();
        workers_and_removal();
        validation_and_reentry();
        cancellation_and_host();
        quota_after_capture_destruction();
        parallel_executor_and_worker_close();
        payload_reentry();
        std::cout << "query stream contracts passed on " << platform_name() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
