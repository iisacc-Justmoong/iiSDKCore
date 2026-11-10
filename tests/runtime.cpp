#include <iiSDKCore/Runtime/EventLoop.h>
#include <iiSDKCore/Runtime/Stream.h>
#include <iiSDKCore/Runtime/Thread.h>

#include <atomic>
#include <array>
#include <chrono>
#include <iostream>
#include <latch>
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

void sparse_mixed_priority_reuse() {
    EventLoop loop(65536);
    std::vector<int> values;
    auto post=[&](int value,Priority priority) { return loop.post([&,value]{values.push_back(value);},priority); };
    (void)post(1,Priority::high);auto h2=post(2,Priority::high);
    (void)post(3,Priority::high);auto n1=post(4,Priority::normal);
    (void)post(5,Priority::normal);(void)post(6,Priority::background);
    (void)post(7,Priority::high);(void)post(8,Priority::high);(void)post(9,Priority::normal);
    check(loop.cancel(h2.token)&&loop.cancel(n1.token),"remove non-tail reservations");
    (void)post(10,Priority::normal);
    auto future=loop.after(1h,[]{throw std::runtime_error("future timer fired");});
    check(loop.poll(generous)==8,"sparse mixed queue dispatch count");
    check(values==std::vector<int>({1,3,7,8,5,9,6,10}),"weighted fairness and FIFO survive cancellation/reuse");
    check(loop.stats().pending==1&&loop.next_deadline().has_value(),"future reservation survives sparse dispatch");
    check(loop.cancel(future.token)&&!loop.next_deadline(),"last cancellation empties deadline state");
    auto invalid=loop.post([]{throw std::runtime_error("invalid priority dispatched");},static_cast<Priority>(255));
    check(loop.poll(generous)==0,"invalid enum priority is never dispatched");
    if(invalid) check(loop.cancel(invalid.token),"invalid priority reservation remains cancellable");
}

void growing_slot_capacity() {
    EventLoop loop(130);
    std::vector<TaskToken> tokens;
    int called=0;
    for (int i=0;i<130;++i) {
        const auto job=loop.post([&]{++called;});
        check(bool(job),"admit work across slot allocation boundaries");
        tokens.push_back(job.token);
    }
    check(loop.post([]{}).error==SubmitError::full,"grown slots preserve the capacity bound");
    for (int index:{0,64,129}) check(tokens[static_cast<std::size_t>(index)].cancel(),"cancel a grown slot");
    for (int i=0;i<3;++i) check(bool(loop.post([&]{++called;})),"reuse a retired grown slot");
    for (int index:{0,64,129}) check(!tokens[static_cast<std::size_t>(index)].pending(),"stale tokens cannot follow reused slots");
    check(loop.poll({256,1s})==130 && called==130 && loop.stats().pending==0,
          "grown and reused slots dispatch and retire exactly once");
}

#if defined(__EMSCRIPTEN__)
void full_wasm_capacity() {
    EventLoop loop(65536);
    std::size_t calls=0;
    for(std::size_t i=0;i<65536;++i)
        check(bool(loop.post([&]{++calls;})),"admit every configured WebAssembly slot");
    check(loop.post([]{}).error==SubmitError::full,"full WebAssembly capacity rejects one extra task");
    check(loop.poll({65536,10s})==65536 && calls==65536 && loop.stats().pending==0,
          "all WebAssembly slots dispatch and retire within the fixed memory limit");
}
#endif

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
    EventLoop release_loop(2); // reentrant work uses a different, already free slot
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

void reactive_selection_changes() {
    EventLoop loop(160);
    auto sender=loop.sender();
    std::vector<TaskToken> dormant;
    for (int i=0;i<128;++i) {
        const auto binding=sender.on_signal([]{});
        check(bool(binding),"admit dormant bindings");
        dormant.push_back(binding.token);
    }
    std::vector<int> order;
    const auto signal=sender.on_signal([&]{order.push_back(2);});
    const auto removed=loop.post([&]{order.push_back(-1);});
    check(signal && removed,"admit reactive selection fixtures");
    check(bool(loop.post([&] {
        order.push_back(1);
        check(removed.token.cancel(),"cancel a cached candidate");
        check(sender.signal(signal.token,Clock::now()),"signal an initially dormant binding");
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
        check(bool(loop.after(0ms,[&]{order.push_back(3);})),"schedule an immediate deadline");
#else
        check(bool(loop.after(1ms,[&]{order.push_back(3);})),"schedule a short deadline");
#endif
        check(bool(loop.post([&]{order.push_back(5);})),"post during dispatch");
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
        std::this_thread::sleep_for(3ms);
#endif
    },Priority::high)),"admit changing callback");
    check(bool(loop.post([&]{order.push_back(4);},Priority::background)),"admit background work");
    check(loop.poll({10,1s})==5 && order==std::vector<int>({1,2,3,4,5}),
          "signals, cancellation, due timers and new work preserve priority ordering");
    check(signal.token.cancel(),"retire reactive binding");
    for (const auto& token:dormant) check(token.cancel(),"retire dormant binding");

    EventLoop staged(4); std::vector<int> staged_order;
    check(bool(staged.post([&]{staged_order.push_back(1);})) &&
          bool(staged.post([&]{staged_order.push_back(2);})),"admit staged work");
    const auto last=staged.post([&]{staged_order.push_back(-1);});
    check(last && staged.poll({1,1s})==1 && staged.poll({1,1s})==1,
          "retain remaining ready work across separate pumps");
    check(last.token.cancel() && bool(staged.post([&]{staged_order.push_back(3);})) &&
          staged.poll({1,1s})==1 && staged_order==std::vector<int>({1,2,3}),
          "revalidate retained candidates after cancellation and slot reuse");
}

void mixed_scheduler_contracts() {
    EventLoop loop(320); auto sender=loop.sender();
    std::vector<TaskToken> dormant;
    for(int i=0;i<256;++i) {
        const auto item=sender.on_signal([]{});
        check(bool(item),"register dormant mixed-work binding");
        dormant.push_back(item.token);
    }
    std::vector<int> order;
    const auto binding=sender.on_signal([&]{order.push_back(5);});
    const auto io=loop.watch(ii::io::Channel{},ii::io::Interest::readable,[&](auto result) {
        check(!result && result.error().code==ii::io::ErrorCode::closed,"closed channel reports its own failure");
        order.push_back(2);
        check(sender.signal(binding.token,Clock::now()),"I/O callback signals its receiver");
    },{.priority=Priority::high});
    RealtimeSchedule schedule{1s,1s,100ms,10ms,Clock::now(),true};
    const auto realtime=sender.realtime(schedule,[&](const auto&) {order.push_back(1);return SliceResult::stop;});
    check(binding && io && realtime && sender.wake_realtime(realtime.token,Clock::now()),
          "register mixed I/O and on-demand realtime work");
    check(bool(loop.post([&]{order.push_back(3);},Priority::high)) &&
          bool(loop.post([&]{order.push_back(4);},Priority::background)),"register mixed priority work");
    check(loop.poll({10,1s})==5 && order==std::vector<int>({1,2,3,5,4}),
          "realtime, I/O, signaled and priority work preserve their ordering");
    check(!io.token.pending() && !realtime.token.pending() && binding.token.cancel(),
          "mixed one-shot and binding lifetimes retire");
    for(const auto& token:dormant) check(token.cancel(),"retire dormant mixed-work binding");

    EventLoop deferred(4); auto deferred_sender=deferred.sender();
    bool ready_io=false,ready_realtime=false;
    schedule.start=Clock::now();
    const auto delayed=deferred_sender.realtime(schedule,[&](const auto&) {
        ready_realtime=true; return SliceResult::stop;
    });
    const auto wake=Clock::now()+1h;
    check(delayed && deferred_sender.wake_realtime(delayed.token,wake),"schedule a future realtime wake");
    check(bool(deferred.watch(ii::io::Channel{},ii::io::Interest::readable,[&](auto result) {
        ready_io=!result && result.error().code==ii::io::ErrorCode::closed;
    })) && deferred.poll({4,1s})==1 && ready_io && !ready_realtime,
          "I/O readiness does not run a future realtime reservation early");
    check(deferred.next_deadline() && *deferred.next_deadline()>=wake,
          "the next deadline retains the future realtime wake");
    check(deferred_sender.wake_realtime(delayed.token,Clock::now()) &&
          deferred.poll({4,1s})==1 && ready_realtime && !delayed.token.pending(),
          "an earlier signal updates the indexed realtime deadline");
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

    EventLoop waiting(2);
    const auto first=waiting.after(1h,[]{}),second=waiting.after(1h,[]{});
    check(first && second,"create independent pending tokens");
    std::latch started(2);
    std::atomic<bool> first_done=false,second_done=false;
    std::thread first_waiter([&] { started.count_down(); first_done=first.token.wait(); });
    std::thread second_waiter([&] { started.count_down(); second_done=second.token.wait(); });
    started.wait();
    const bool first_cancelled=first.token.cancel();
    first_waiter.join();
    const bool second_still_pending=!second_done && second.token.pending();
    const bool second_cancelled=second.token.cancel();
    second_waiter.join();
    check(first_cancelled && first_done && second_still_pending && second_cancelled && second_done,
          "independent token waiters retire only their own work");
}
#endif

int main() {
    try {
        queue_and_tokens(); timers_and_budgets(); sparse_mixed_priority_reuse(); growing_slot_capacity(); fairness_and_failures(); reactive_selection_changes(); mixed_scheduler_contracts(); stream_contract();
#if defined(__EMSCRIPTEN__)
        full_wasm_capacity();
#endif
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
