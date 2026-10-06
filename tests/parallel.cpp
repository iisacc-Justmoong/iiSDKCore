#include <iiSDKCore/Runtime/Runtime.h>
#include <iiSDKCore/Runtime/ObjectProperties.h>
#include <atomic>
#include <array>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>

using namespace ii::runtime;
using namespace std::chrono_literals;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

// A bounded rendezvous demonstrates overlapping execution without speed benchmarks.
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    int entered = 0;
    bool released = false;
    void enter() {
        std::unique_lock lock(mutex);
        ++entered;
        changed.notify_all();
        if (!changed.wait_for(lock, 5s, [&] { return released; })) throw std::runtime_error("gate timed out");
    }
    bool wait(int count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return entered >= count; });
    }
    void release() { std::lock_guard lock(mutex); released = true; changed.notify_all(); }
};

int main() {
    try {
        if (!Thread::supported()) {
            bool rejected = false;
            try { Runtime runtime; } catch (const std::exception&) { rejected = true; }
            check(rejected, "parallel runtime never silently falls back to inline execution");
            std::cout << "parallel runtime unsupported contract passed\n";
            return 0;
        }
        Runtime runtime;
        check(runtime.stats().workers >= 2, "default runtime owns multiple workers");
        Gate parallel;
        const auto caller = Thread::current_id();
        std::atomic<int> background = 0;
        for (int i = 0; i < 2; ++i) check(bool(runtime.post([&] {
            if (Thread::current_id() != caller) ++background;
            parallel.enter();
        })), "submit parallel work");
        const bool overlap = parallel.wait(2);
        parallel.release();
        check(bool(runtime.wait_idle()) && overlap && background == 2, "default tasks run concurrently off caller");

        SerialQueue first(runtime), second(runtime);
        Gate objects;
        check(bool(first.post([&] { objects.enter(); })) && bool(second.post([&] { objects.enter(); })), "independent objects");
        const bool object_overlap = objects.wait(2);
        objects.release();
        check(bool(runtime.wait_idle()) && object_overlap, "different object queues run in parallel");
        int serial_value = 0;
        std::atomic<int> active = 0, max_active = 0;
        for (int i = 0; i < 200; ++i) check(bool(first.post([&, i] {
            const int count = ++active;
            if (count > max_active) max_active = count;
            check(first.is_current() && !second.is_current(), "current execution queue identity");
            check(serial_value == i, "FIFO order across priorities");
            ++serial_value;
            --active;
        }, i % 2 ? Priority::high : Priority::background)), "submit serialized work");
        check(bool(runtime.wait_idle()) && serial_value == 200 && max_active == 1, "one object has no concurrent mutation");

        std::array<Thread, 4> producers;
        int multi_producer_count = 0;
        for (auto& producer : producers) check(bool(producer.start([&](ThreadContext&) {
            for (int i = 0; i < 100; ++i) check(bool(first.post([&] { ++multi_producer_count; })), "multi-producer admission");
        })), "producer starts");
        for (auto& producer : producers) check(bool(producer.join()), "producer joined");
        check(bool(runtime.wait_idle()) && multi_producer_count == 400, "concurrent senders share an object queue safely");

        QueryPort<int, int> port(first, [&](const int& n) {
            serial_value += n;
            return serial_value;
        });
        auto direct = port.send(1);
        check(!direct && direct.error().code == QueryErrorCode::wrong_executor, "bound query rejects foreign synchronous access");
        int response = 0;
        check(bool(port.post(3, [&](QueryReply<int> reply) {
            check(first.is_current(), "completion retains object queue");
            response = reply.value();
        })), "bound query defaults to async delivery");
        check(bool(runtime.wait_idle()) && response == 203, "bound query completed asynchronously");
        check(port.post(runtime, 1, [](QueryReply<int>) {}).error == SubmitError::invalid,
              "bound query cannot bypass serialization through another executor");
        check(bool(first.post([&] { check(port.send(1).value() == 204, "same-queue direct call"); })), "internal object call");
        check(bool(runtime.wait_idle()), "internal query completed");

        ObjectProperties properties(first, {{.name = "value", .value = 0,
            .policy = {.queryable = true, .writable = true}}});
        auto foreign = properties.apply(PropertyWrite{{"value"}, 1});
        check(!foreign && foreign.error().code == PropertyErrorCode::wrong_executor, "bound property owner also checks execution context");
        std::atomic<int> replies = 0;
        for (int n = 1; n <= 100; ++n) check(bool(properties.queries().post(PropertyWrite{{"value"}, n},
            [&](QueryReply<PropertyResult> reply) { check(reply && *reply, "async property edit"); ++replies; })), "post property edit");
        check(bool(runtime.wait_idle()) && replies == 100 && properties.revision() == 100, "serialized dynamic instance changes");

        Runtime bounded({.workers = 2, .capacity = 3});
        Gate busy;
        check(bool(bounded.post([&] { busy.enter(); })) && bool(bounded.post([&] { busy.enter(); })), "occupy workers");
        check(busy.wait(2), "both workers entered");
        bool cancelled_ran = false;
        auto queued = bounded.post([&] { cancelled_ran = true; });
        check(bool(queued) && bounded.post([] {}).error == SubmitError::full, "shared capacity includes active jobs");
        EventLoop unrelated;
        check(!runtime.cancel(queued.token) && !unrelated.cancel(queued.token), "tokens are scheduler-specific");
        check(bounded.cancel(queued.token) && !bounded.cancel(queued.token), "cancel queued task exactly once");
        auto replacement = bounded.post([] {});
        check(bool(replacement) && !bounded.cancel(queued.token), "reused slot rejects stale generation");
        busy.release();
        check(bool(bounded.wait_idle()) && !cancelled_ran, "cancel prevents execution");

        Gate chain;
        std::vector<int> order;
        check(bool(first.post([&] { chain.enter(); })) && chain.wait(1), "hold serial chain");
        auto head = first.post([&] { order.push_back(1); });
        check(bool(first.post([&] { order.push_back(2); })), "chain second");
        auto middle = first.post([&] { order.push_back(3); });
        check(bool(first.post([&] { order.push_back(4); })), "chain last");
        check(runtime.cancel(head.token) && runtime.cancel(middle.token), "unlink serial head and middle");
        chain.release();
        check(bool(runtime.wait_idle()) && order == std::vector<int>({2, 4}), "cancellation preserves remaining FIFO");

        {
            Runtime cancelling({.workers = 2, .capacity = 4});
            Gate active_jobs;
            std::atomic<int> unwanted = 0;
            check(bool(cancelling.post([&] { active_jobs.enter(); })) &&
                  bool(cancelling.post([&] { active_jobs.enter(); })) && active_jobs.wait(2), "hold cancellation workers");
            check(bool(cancelling.post([&] { ++unwanted; })) && bool(cancelling.post([&] { ++unwanted; })), "queued cancellation jobs");
            cancelling.request_stop(ShutdownMode::cancel_pending);
            check(cancelling.post([] {}).error == SubmitError::stopped, "shutdown closes admission");
            active_jobs.release();
            check(bool(cancelling.join()) && unwanted == 0 && cancelling.stats().cancelled == 2 &&
                  cancelling.stats().pending == 0, "cancel-pending leaves active jobs to finish");
        }

        bool expired = false;
        Gate held;
        check(bool(first.post([&] { held.enter(); })) && held.wait(1), "hold object queue");
        {
            QueryPort<int> temporary(first, [](const int&) {});
            check(bool(temporary.post(1, [&](QueryReply<void> reply) {
                expired = true;
                (void)reply;
            })), "queue query before object destruction");
        }
        held.release();
        check(bool(runtime.wait_idle()) && !expired, "removed query port leaves no pending completion");

        Runtime failures({.workers = 2, .capacity = 8});
        std::atomic<int> after_failure = 0;
        check(bool(failures.post([] { throw std::runtime_error("parallel task"); })), "throwing job accepted");
        check(bool(failures.post([&] { ++after_failure; })), "next job accepted");
        auto failed = failures.wait_idle();
        check(!failed && failed.error().code == ThreadErrorCode::work_failed && failed.error().cause &&
              failures.stats().failed == 1 && after_failure == 1, "worker survives task failure and reports it");
        check(!failures.join(), "join retains task failure");

        bool self_wait = false;
        check(bool(runtime.post([&] {
            auto result = runtime.wait_idle();
            self_wait = !result && result.error().code == ThreadErrorCode::self_join;
        })), "self wait probe");
        check(bool(runtime.wait_idle()) && self_wait, "pool workers cannot deadlock waiting on their own pool");

        std::unique_ptr<SerialQueue> surviving;
        int drained = 0;
        {
            Runtime owned({.workers = 2, .capacity = 8});
            surviving = std::make_unique<SerialQueue>(owned);
            for (int i = 0; i < 8; ++i) check(bool(surviving->post([&] { ++drained; })), "RAII drain submission");
        }
        check(drained == 8 && surviving->post([] {}).error == SubmitError::stopped, "RAII drain and expired scheduler");
        check(bool(runtime.join()) && runtime.post([] {}).error == SubmitError::stopped, "join closes admission and drains");
        std::cout << "parallel runtime contracts passed on " << platform_name() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
