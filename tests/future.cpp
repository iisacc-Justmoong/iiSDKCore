#include <iiSDKCore/Runtime/Future.h>
#include <iiSDKCore/Runtime/FutureWatcher.h>
#include <iiSDKCore/Runtime/FutureSynchronizer.h>
#include <atomic>
#include <iostream>
#include <latch>
#include <future>
#include <stdexcept>

using namespace ii::runtime;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void values_and_lifetime() {
    Future<int> invalid;
    check(!invalid.valid() && invalid.wait_for(0ms) == FutureWait::invalid,
          "empty futures are explicit");
    Promise<int> promise({.max_results = 3});
    auto future = promise.future();
    check(promise.start() && !promise.start(), "start is one transition");
    check(bool(promise.add_result(30, 2)) && bool(promise.add_result(10, 0)),
          "indexed results allow out-of-order producers");
    check(!future.result_at(1) && future.snapshot().result_count == 2,
          "result availability is distinct from index");
    check(!promise.add_result(99, 0) && !promise.add_result(99, 3),
          "duplicate and over-budget results are rejected");
    check(bool(promise.set_progress_range(10, 20)) &&
          bool(promise.set_progress(15, "half")) && !promise.set_progress(21),
          "progress is range checked");
    check(future.wait_for(0ms) == FutureWait::timed_out && !future.ready(),
          "observer timeout does not finish work");
    check(bool(promise.add_result(20, 1)) && promise.finish() && !promise.finish(),
          "finish is exactly once");
    auto values = future.get_all();
    check(values && *values == std::vector<int>({10, 20, 30}) &&
          future.snapshot().status == FutureStatus::succeeded &&
          !future.cancel() && !promise.add_result(40), "ordered immutable results");
    check(future.snapshot().progress.text == "half", "progress text survives completion");
    Promise<std::unique_ptr<int>> movable;
    auto moved = movable.future();
    check(bool(movable.add_result(std::make_unique<int>(42))) && movable.finish() &&
          **moved.result_at(0) == 42, "move-only values have shared const access");
    Future<int> abandoned;
    {
        Promise<int> producer;
        abandoned = producer.future();
    }
    check(abandoned.ready() && abandoned.snapshot().status == FutureStatus::abandoned &&
          !abandoned.get(), "producer removal settles its future");
    Promise<void> empty;
    auto done = empty.future();
    check(empty.finish() && bool(done.get()), "void futures have no fake value");
    Promise<int> failed;
    auto failure = failed.future();
    check(failed.fail({ii::core::ErrorCode::failed, "fixture", {}}) &&
          failure.snapshot().status == FutureStatus::failed && !failure.get(),
          "structured failures survive observation");
    Promise<int> transferred;
    auto survivor = transferred.future();
    Promise<int> destination(std::move(transferred));
    check(bool(destination.add_result(7)) && destination.finish() && *survivor.get() == 7,
          "moving the producer preserves the future");
}
void watchers() {
    EventLoop replies(4);
    Promise<int> promise;
    auto future = promise.future();
    int calls = 0;
    FutureSnapshot last;
    FutureWatcher<int> watcher(future, replies, [&](const FutureSnapshot& snapshot) {
        ++calls; last = snapshot;
    });
    check(promise.start(), "watched work starts");
    for (std::size_t i = 0; i <= 100; ++i)
        check(bool(promise.set_progress(i)), "watched progress");
    check(bool(promise.add_result(42)) && promise.finish(), "watched completion");
    check(calls == 0 && replies.stats().pending == 1, "notifications coalesce asynchronously");
    (void)replies.poll();
    check(calls == 1 && last.status == FutureStatus::succeeded && last.result_count == 1,
          "coalesced delivery includes the latest terminal snapshot");
    check(bool(watcher.close()), "watcher removal barrier");
    {
        EventLoop full(1);
        auto occupying = full.post([] {});
        check(bool(occupying), "occupy observer executor");
        Promise<void> task;
        int count = 0;
        FutureWatcher<void> pending(task.future(), full, [&](const auto&) { ++count; });
        check(pending.last_submission() == SubmitError::full && task.finish(),
              "observer backpressure is visible");
        (void)full.poll();
        check(pending.retry() == SubmitError::none, "observer admission can be retried");
        (void)full.poll();
        check(count == 1, "retry delivers terminal state");
    }
    {
        EventLoop loop;
        Promise<void> task;
        int count = 0;
        FutureWatcher<void> reentrant(task.future(), loop, [owned = std::make_unique<int>(1), &task, &count](const auto& snapshot) mutable {
            count += *owned;
            if (snapshot.status == FutureStatus::pending) (void)task.finish();
        });
        (void)loop.poll();
        check(count == 2, "move-only observer can change its producer without holding a future lock");
    }
    {
        EventLoop loop;
        Promise<void> task;
        int count = 0;
        {
            FutureWatcher<void> removed(task.future(), loop, [&](const auto&) { ++count; });
            check(task.finish(), "finish before watcher removal");
        }
        (void)loop.poll();
        check(count == 0 && loop.stats().pending == 0, "removed watcher suppresses queued callbacks");
    }
    {
        EventLoop loop;
        Promise<void> task;
        FutureWatcher<void> discarded(task.future(), loop, [](const auto&) {});
        loop.request_stop();
        check(discarded.retry() == SubmitError::stopped,
              "discarded observer tasks release their scheduling flag");
    }
    {
        EventLoop loop;
        Promise<void> task;
        std::vector<std::unique_ptr<FutureWatcher<void>>> observers;
        for (int i = 0; i < 16; ++i)
            observers.push_back(std::make_unique<FutureWatcher<void>>(task.future(), loop, [](const auto&) {}));
        bool limited = false;
        try { FutureWatcher<void> overflow(task.future(), loop, [](const auto&) {}); }
        catch (const std::length_error&) { limited = true; }
        check(limited, "observer capacity is bounded");
        observers.clear();
        FutureWatcher<void> reused(task.future(), loop, [](const auto&) {});
        check(task.finish(), "observer slots are reusable");
    }
}
void cancellation_and_waits() {
    Promise<int> promise;
    auto future = promise.future();
    check(future.cancel() && !future.cancel() && !future.ready(),
          "cancellation is a request until producer settlement");
    check(!promise.checkpoint() && !promise.add_result(1) && promise.finish() &&
          future.snapshot().status == FutureStatus::cancelled, "cooperative cancellation");
    if (!Thread::supported()) return;
    Runtime workers({.workers = 2, .capacity = 8});
    Promise<int> work;
    auto observing = work.future();
    check(observing.suspend(), "request suspension before checkpoint");
    std::latch entered(1);
    auto submission = workers.post([&] {
        check(observing.wait_for(1ms) == FutureWait::would_block &&
              !observing.get(), "workers cannot block on futures");
        entered.count_down();
        if (work.checkpoint()) {
            (void)work.add_result(9);
        }
        (void)work.finish();
    });
    check(bool(submission), "checkpoint job accepted");
    entered.wait();
    const auto deadline = Clock::now() + 5s;
    while (!observing.snapshot().suspended_producers && Clock::now() < deadline)
        std::this_thread::yield();
    check(observing.snapshot().suspended_producers == 1 && !observing.ready(),
          "suspension is acknowledged at a checkpoint");
    check(observing.cancel() && observing.wait_for(5s) == FutureWait::ready &&
          observing.snapshot().status == FutureStatus::cancelled,
          "cancellation wakes a suspended producer");
    check(bool(workers.wait_idle()), "producer retired");
    {
        Promise<int> resumed;
        const auto future = resumed.future();
        check(future.suspend() && bool(workers.post([&] {
            if (resumed.checkpoint()) (void)resumed.add_result(9);
            (void)resumed.finish();
        })), "resumable producer accepted");
        const auto until = Clock::now() + 5s;
        while (!future.snapshot().suspended_producers && Clock::now() < until) std::this_thread::yield();
        check(future.snapshot().suspended_producers == 1 && future.resume() && *future.get() == 9,
              "resume wakes a suspended producer");
        check(bool(workers.wait_idle()), "resumed producer retired");
    }
    {
        Promise<int> shared({.max_results = 256});
        const auto future = shared.future();
        for (int producer = 0; producer < 4; ++producer)
            check(bool(workers.post([&, producer] {
                for (int i = 0; i < 64; ++i) check(bool(shared.add_result(producer * 64 + i)), "concurrent append");
            })), "shared producer job");
        check(bool(workers.wait_idle()) && shared.finish() && future.get_all()->size() == 256,
              "concurrent result append is synchronized");
    }
    {
        Promise<void> producer;
        std::latch entered(1), release(1), closing(1);
        auto capture = std::make_shared<int>(1);
        std::weak_ptr<int> weak = capture;
        FutureWatcher<void> observer(producer.future(), workers, [capture, &entered, &release](const auto&) {
            entered.count_down(); release.wait();
        });
        capture.reset();
        entered.wait();
        auto barrier = std::async(std::launch::async, [&] { closing.count_down(); return observer.close(); });
        closing.wait();
        check(barrier.wait_for(1ms) != std::future_status::ready && !weak.expired(),
              "watcher removal waits for the active handler");
        release.count_down();
        check(bool(barrier.get()) && weak.expired(), "watcher removal releases handler captures");
    }
    Promise<int> a, b;
    FutureSynchronizer<int> group(2);
    check(bool(group.add(a.future())) && bool(group.add(b.future())) &&
          !group.add(a.future()), "synchronizer budget");
    check(a.finish() && group.wait_for(0ms) == FutureWait::timed_out,
          "synchronizer waits for every future");
    group.cancel();
    check(b.finish() && group.wait_for(1s) == FutureWait::ready,
          "synchronizer settles cancelled producers too");
}
}
int main() {
    try {
        values_and_lifetime(); watchers(); cancellation_and_waits();
        std::cout << "future, promise, watcher and synchronizer contracts passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
