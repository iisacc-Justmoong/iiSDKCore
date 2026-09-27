#include <iiSDKCore/Runtime/EventLoop.h>
#include <iiSDKCore/Runtime/Stream.h>
#include <iiSDKCore/Runtime/Thread.h>

#include <atomic>
#include <array>
#include <chrono>
#include <iostream>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace ii::runtime;
using namespace std::chrono_literals;

void check(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}
constexpr PumpBudget generous{1024, 1s};

void queue_and_tokens() {
    EventLoop loop(2), other(2);
    std::vector<int> values;
    auto first = loop.post([&] { values.push_back(1); });
    auto second = loop.post([&] { values.push_back(2); });
    check(bool(first) && bool(second), "submit capacity");
    check(loop.post([] {}).error == SubmitError::full, "explicit backpressure");
    check(!other.cancel(first.token), "cross-loop token rejected");
    check(loop.cancel(first.token), "queued task cancellation");
    auto third = loop.post([&] { values.push_back(3); });
    check(bool(third) && !loop.cancel(first.token), "slot reuse keeps token identity");
    check(loop.poll(generous) == 2, "dispatch count");
    check(values == std::vector<int>({2, 3}), "FIFO despite slot reuse");
    for (int i = 0; i < 10000; ++i) {
        check(bool(loop.post([] {})), "reused queue accepts task");
        loop.poll(generous);
    }
    check(loop.stats().capacity == 2 && loop.stats().high_watermark == 2,
          "queue stays bounded after warmup");
    check(loop.post({}).error == SubmitError::invalid, "empty callback rejected");
    check(loop.every(0ns, [] {}).error == SubmitError::invalid, "zero period rejected");
}

void timers_and_budgets() {
    EventLoop loop;
    int count = 0;
    auto timer = loop.at(Clock::now() + 1h, [&] { ++count; });
    check(loop.poll(generous) == 0, "future timer does not fire early");
    check(loop.next_deadline().has_value(), "host can inspect next deadline");
    check(loop.cancel(timer.token), "timer cancellation");
    for (int i = 0; i < 5; ++i) (void)loop.post([&] { ++count; });
    check(loop.poll({2, 1s}) == 2 && count == 2, "callback budget");
    check(loop.poll({20, 0ns}) == 0, "zero time budget");
    loop.suspend();
    check(loop.poll(generous) == 0, "suspension preserves queue");
    loop.resume();
    check(loop.poll(generous) == 3, "resume dispatches preserved queue");
    loop.request_stop();
    check(loop.poll(generous) == 0, "stopped loop does not dispatch");
    check(loop.post([] {}).error == SubmitError::stopped, "stop closes admission");
    loop.resume();
    check(loop.state() == LoopState::stopped, "stop is terminal");
}

void fairness_and_failures() {
    EventLoop loop;
    int background = 0;
    for (int i = 0; i < 30; ++i) (void)loop.post([] {}, Priority::high);
    (void)loop.post([&] { ++background; }, Priority::background);
    loop.poll({7, 1s});
    check(background == 1, "background cannot starve behind high priority");
    loop.poll(generous);
    (void)loop.post([&] {
        bool rejected = false;
        try { loop.poll(); } catch (const std::logic_error&) { rejected = true; }
        check(rejected, "recursive pump must fail");
    });
    loop.poll(generous);
    (void)loop.post([] { throw std::runtime_error("callback failure"); });
    bool propagated = false;
    try { loop.poll(generous); } catch (const std::runtime_error&) { propagated = true; }
    check(propagated && loop.stats().pending == 0, "exception cleanup and propagation");
    (void)loop.post([] {});
    check(loop.poll(generous) == 1, "dispatch recovers after exception");

    // Releasing user captures must not hold the queue lock.
    bool destroyed = false;
    bool destructor_posted = false;
    EventLoop release_loop(1);
    auto capture = std::shared_ptr<int>(new int(1), [&](int* value) {
        delete value;
        destroyed = true;
        destructor_posted = bool(release_loop.post([] {}));
    });
    auto pending = release_loop.post([capture] {});
    capture.reset();
    check(release_loop.cancel(pending.token) && destroyed, "capture released outside lock");
    check(destructor_posted, "capture destructor may enqueue");
    check(release_loop.poll(generous) == 1, "destructor-enqueued task runs");
}

void stream_contract() {
    Stream<int> stream(2);
    check(stream.try_push(1) && stream.try_push(2), "stream accepts capacity");
    check(!stream.try_push(3), "stream backpressure");
    int value = 0;
    check(stream.try_pop(value) && value == 1, "stream FIFO");
    check(stream.try_push(3), "stream reuses released storage");
    stream.close();
    check(!stream.try_push(4), "closed stream rejects producer");
    check(stream.try_pop(value) && value == 2, "close allows draining");
    check(stream.try_pop(value) && value == 3, "drain last value");
    check(!stream.try_pop(value) && stream.drained(), "end of stream");
    Stream<int> live(2, OverflowPolicy::drop_oldest);
    check(live.try_push(1) && live.try_push(2) && live.try_push(3), "live stream");
    check(live.stats().dropped == 1, "drop is observable");
    check(live.try_pop(value) && value == 2, "live stream retains recent samples");
}

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
void threaded_stream() {
    Stream<int> stream(8);
    std::vector<std::atomic<int>> seen(400);
    std::atomic<bool> invalid = false;
    auto consume = [&](ThreadContext&) {
        int value = 0;
        while (!stream.drained()) {
            if (stream.try_pop(value)) {
                if (value < 0 || value >= 400) invalid = true;
                else ++seen[static_cast<std::size_t>(value)];
            } else std::this_thread::yield();
        }
    };
    Thread first, second;
    check(bool(first.start(consume)) && bool(second.start(consume)), "managed stream consumers");
    std::array<Thread, 4> producers;
    for (int p = 0; p < 4; ++p) check(bool(producers[p].start([&, p](ThreadContext&) {
        for (int i = 0; i < 100; ++i)
            while (!stream.try_push(p * 100 + i)) std::this_thread::yield();
    })), "managed stream producer");
    for (auto& producer : producers) check(bool(producer.join()), "stream producer joined");
    stream.close();
    check(bool(first.join()) && bool(second.join()), "stream consumers joined");
    check(!invalid, "MPMC stream payload integrity");
    for (const auto& count : seen) check(count == 1, "MPMC stream exactly-once delivery");
}

void threaded_driver() {
    EventLoop loop;
    std::atomic<int> count = 0;
    Thread runner;
    check(bool(runner.start(loop)), "managed event loop");
    std::array<Thread, 4> producers;
    for (auto& producer : producers) check(bool(producer.start([&](ThreadContext&) {
        for (int i = 0; i < 100; ++i)
            check(bool(loop.post([&] { ++count; })), "concurrent producer");
    })), "managed query producer");
    for (auto& producer : producers) check(bool(producer.join()), "query producer joined");
    (void)loop.post([&] { loop.request_stop(); });
    check(bool(runner.join()), "event loop joined");
    check(count == 400, "cross-thread wake and serialized dispatch");

    EventLoop timed;
    int ticks = 0;
    TaskToken token;
    token = timed.every(1ms, [&] {
        ++ticks;
        if (ticks == 1) std::this_thread::sleep_for(8ms);
        if (ticks == 3) {
            check(timed.cancel(token), "cancel active periodic callback");
            timed.request_stop();
        }
    }).token;
    timed.run();
    check(ticks == 3 && timed.stats().missed_ticks >= 1, "skip missed ticks without burst");
    check(timed.stats().pending == 0, "active cancellation releases slot");

    EventLoop suspended;
    suspended.suspend();
    std::atomic<bool> delivered = false;
    (void)suspended.post([&] { delivered = true; suspended.request_stop(); });
    Thread suspended_runner;
    check(bool(suspended_runner.start(suspended)), "managed suspended loop");
    suspended.resume();
    check(bool(suspended_runner.join()), "resumed loop joined");
    check(delivered, "resume wakes sleeping driver without losing work");

    EventLoop idle;
    Thread idle_runner;
    check(bool(idle_runner.start(idle)), "managed idle loop");
    idle.request_stop();
    check(bool(idle_runner.join()), "idle loop joined");
    check(idle.state() == LoopState::stopped, "stop wakes idle driver");

    EventLoop concurrent;
    std::mutex gate;
    std::condition_variable ready;
    bool entered = false, finish = false;
    auto active = concurrent.post([&] {
        std::unique_lock lock(gate);
        entered = true;
        ready.notify_one();
        ready.wait(lock, [&] { return finish; });
    });
    Thread dispatcher;
    check(bool(dispatcher.start([&](ThreadContext&) { concurrent.poll(generous); })), "managed dispatcher");
    {
        std::unique_lock lock(gate);
        ready.wait(lock, [&] { return entered; });
    }
    bool concurrent_rejected = false;
    try { concurrent.poll(); } catch (const std::logic_error&) { concurrent_rejected = true; }
    const bool cancelled = concurrent.cancel(active.token);
    {
        std::lock_guard lock(gate);
        finish = true;
    }
    ready.notify_one();
    check(bool(dispatcher.join()), "dispatcher joined");
    check(concurrent_rejected && cancelled, "concurrent pump rejected; active task cancellable");
    check(concurrent.stats().pending == 0, "active task releases reservation on return");
}
#endif

int main() {
    try {
        queue_and_tokens(); timers_and_budgets(); fairness_and_failures(); stream_contract();
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
        threaded_driver(); threaded_stream();
#endif
        std::cout << "runtime contracts passed on " << platform_name() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
