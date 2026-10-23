#include <iiSDKCore/Runtime/Concurrent.h>
#include <iiSDKCore/Runtime/FutureWatcher.h>
#include <atomic>
#include <iostream>
#include <latch>
#include <numeric>
#include <stdexcept>

using namespace ii::runtime;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void computation() {
    Runtime workers({.workers = 4, .capacity = 32});
    Concurrent concurrent(workers);
    const auto caller = Thread::current_id();
    auto single = concurrent.run([caller](std::unique_ptr<int> value) {
        check(Thread::current_id() != caller, "run uses persistent workers");
        return *value + 1;
    }, std::make_unique<int>(41));
    check(single.wait_for(5s) == FutureWait::ready && *single.get() == 42,
          "owned arguments and scalar return");
    auto empty = concurrent.run([] {});
    check(bool(empty.get()), "void run");
    auto error = concurrent.run([]() -> int { throw std::runtime_error("fixture"); });
    check(!error.get() && error.snapshot().error->cause && bool(workers.wait_idle()),
          "work exceptions belong to the future and do not poison the runtime");
    auto stream = concurrent.run_with_promise<int>([](Promise<int>& promise) {
        (void)promise.set_progress_range(0, 3);
        for (int i = 0; i < 3; ++i) {
            if (!promise.checkpoint()) return;
            (void)promise.add_result(i * 10);
            (void)promise.set_progress(i + 1);
        }
    });
    check(*stream.get_all() == std::vector<int>({0, 10, 20}) &&
          stream.snapshot().progress.value == 3, "promise mode reports multiple results and progress");
    auto built = task([](int a, int b) { return a + b; })
        .with_arguments(20, 22).with_priority(Priority::high).spawn(concurrent);
    check(*built.get() == 42, "configured task builder");
    std::vector<int> input(257);
    std::iota(input.begin(), input.end(), 0);
    auto mapped = concurrent.mapped(input, [](int x) { return x * 2; }, {.grain = 8});
    auto all = mapped.get_all();
    check(all && all->size() == input.size(), "parallel map result count");
    for (std::size_t i = 0; i < all->size(); ++i)
        check((*all)[i] == input[i] * 2, "map preserves input ordering");
    check(mapped.snapshot().progress.value == input.size(), "parallel progress");
    auto kept = concurrent.filtered(input, [](int x) { return x % 2 == 0; }, {.grain = 4});
    auto values = kept.get_all();
    check(values && values->size() == 129 && values->front() == 0 && values->back() == 256,
          "filter preserves source ordering");
    check(*concurrent.filtered(std::vector{1, 2, 3}, [](int x) { return x % 2 == 0; }).get() == 2,
          "get returns the first available filtered result");
    auto edited = concurrent.map(std::vector{1, 2, 3}, [](int& value) { value *= 3; });
    check(*edited.get() == std::vector<int>({3, 6, 9}), "owned in-place map");
    auto compacted = concurrent.filter(std::vector{1, 2, 3, 4}, [](int value) { return value % 2; });
    check(*compacted.get() == std::vector<int>({1, 3}), "owned in-place filter");
    std::atomic<int> reducing = 0;
    auto reduced = concurrent.mapped_reduced(std::vector{1, 2, 3, 4},
        [](int x) { return std::to_string(x); }, [&](std::string& total, std::string value) {
            check(Thread::current_id() != caller && reducing.fetch_add(1) == 0,
                  "reducer is serialized on a worker");
            total += value; --reducing;
        }, std::string{} , {.grain = 1});
    check(*reduced.get() == "1234", "ordered sequential reduction");
    auto sum = concurrent.filtered_reduced(std::vector{1, 2, 3, 4},
        [](int value) { return value % 2 == 0; }, [](int& total, int value) { total += value; }, 0);
    check(*sum.get() == 6, "filter reduce");
    check(concurrent.mapped(std::vector<int>{}, [](int x) { return x; }).get_all()->empty(),
          "empty batches settle");
    check(!concurrent.mapped(input, [](int x) { return x; }, {.grain = 0}).get_all(),
          "zero grain is an explicit failure");
    auto map_error = concurrent.mapped(input, [](int) -> int { throw std::runtime_error("map fixture"); });
    check(!map_error.get_all() && map_error.snapshot().status == FutureStatus::failed,
          "mapper failure settles the entire batch");
    auto reduce_error = concurrent.mapped_reduced(std::vector{1}, [](int x) { return x; },
        [](int&, int) { throw std::runtime_error("reduce fixture"); }, 0);
    check(!reduce_error.get() && reduce_error.snapshot().status == FutureStatus::failed,
          "reducer failure settles the batch");
    check(*concurrent.blocking_mapped(std::vector{2, 3}, [](int x) { return x * x; }) ==
          std::vector<int>({4, 9}), "blocking map returns structured results");
    check(*concurrent.blocking_filtered(std::vector{2, 3}, [](int x) { return x % 2; }) ==
          std::vector<int>({3}), "blocking filter");
    check(*concurrent.blocking_map(std::vector{2, 3}, [](int& x) { ++x; }) == std::vector<int>({3, 4}) &&
          *concurrent.blocking_filter(std::vector{2, 3}, [](int x) { return x % 2; }) == std::vector<int>({3}),
          "blocking owned mutation");
    check(*concurrent.blocking_mapped_reduced(std::vector{2, 3}, [](int x) { return x * x; },
          [](int& a, int b) { a += b; }, 0) == 13 &&
          *concurrent.blocking_filtered_reduced(std::vector{2, 3}, [](int x) { return x % 2; },
          [](int& a, int b) { a += b; }, 0) == 3, "blocking reductions");
    auto bits = concurrent.mapped(std::vector<bool>{true, false, true}, [](bool x) { return !x; }, {.grain = 1});
    check(*bits.get_all() == std::vector<bool>({false, true, false}), "mapped bool has independent result storage");
    {
        std::vector<std::unique_ptr<int>> owned;
        owned.push_back(std::make_unique<int>(42));
        auto output = concurrent.filtered(std::move(owned), [](const auto& x) { return *x == 42; });
        check(output.wait_for(5s) == FutureWait::ready && **output.result_at(0) == 42,
              "filter owns move-only inputs and outputs");
    }
    {
        std::latch entered(4), release(1);
        auto parallel = concurrent.mapped(std::vector{1, 2, 3, 4}, [&](int x) {
            entered.count_down(); release.wait(); return x;
        }, {.grain = 1});
        entered.wait(); release.count_down();
        check(*parallel.get_all() == std::vector<int>({1, 2, 3, 4}), "four independent chunks execute concurrently");
    }
    std::atomic<int> forbidden_calls = 0;
    auto nonblocking = concurrent.run([&] {
        auto gate = std::make_shared<std::latch>(1);
        auto blocked = concurrent.blocking_mapped(std::vector{1}, [gate, &forbidden_calls](int x) {
            gate->wait(); ++forbidden_calls; return x;
        });
        gate->count_down();
        return !blocked && blocked.error().code == ii::core::ErrorCode::wrong_executor;
    });
    check(*nonblocking.get() && bool(workers.wait_idle()) && forbidden_calls == 0,
          "blocking variants reject a pool worker before submitting borrowed work");
    check(bool(concurrent.close()) && bool(workers.wait_idle()), "close and capture retirement");
    auto stopped = concurrent.run([] { return 1; });
    check(stopped.snapshot().status == FutureStatus::rejected &&
          stopped.snapshot().admission == SubmitError::stopped, "closed owner rejects work");
}
void cancellation_and_shutdown() {
    Runtime workers({.workers = 1, .capacity = 4});
    Concurrent concurrent(workers);
    std::latch occupied(1), release(1);
    check(bool(workers.post([&] { occupied.count_down(); release.wait(); })), "occupy worker");
    occupied.wait();
    int unwanted = 0;
    auto queued = concurrent.run([&] { ++unwanted; return 1; });
    check(queued.cancel() && queued.wait_for(1s) == FutureWait::ready &&
          queued.snapshot().status == FutureStatus::cancelled && unwanted == 0,
          "queued cancellation settles without running work");
    auto pending = concurrent.mapped(std::vector{1, 2, 3}, [](int x) { return x; }, {.grain = 1});
    workers.request_stop(ShutdownMode::cancel_pending);
    check(pending.wait_for(1s) == FutureWait::ready && !pending.get_all(),
          "executor discard settles all chunk tickets");
    release.count_down();
    check(bool(workers.join()) && bool(concurrent.close()), "cancel-pending shutdown");
    {
        Runtime bounded({.workers = 1, .capacity = 1});
        Concurrent jobs(bounded);
        std::latch held(1), free(1);
        check(bool(bounded.post([&] { held.count_down(); free.wait(); })), "bounded pool hold");
        held.wait();
        auto rejected = jobs.run([] { return 1; });
        auto rejected_batch = jobs.mapped(std::vector{1, 2}, [](int x) { return x; });
        check(rejected.snapshot().status == FutureStatus::rejected &&
              rejected.snapshot().admission == SubmitError::full && rejected_batch.ready() &&
              rejected_batch.snapshot().admission == SubmitError::full, "bounded admission is observable");
        free.count_down();
        check(bool(bounded.wait_idle()), "blocking fixture retires before its latches leave scope");
    }
    {
        Runtime pool({.workers = 1, .capacity = 4});
        Concurrent jobs(pool);
        std::latch entered(1), release(1);
        std::atomic<int> calls = 0;
        auto active = jobs.mapped(std::vector{1, 2, 3, 4}, [&](int x) {
            ++calls; entered.count_down(); release.wait(); return x;
        });
        entered.wait(); check(active.cancel(), "cancel active batch"); release.count_down();
        check(active.wait_for(5s) == FutureWait::ready && active.snapshot().status == FutureStatus::cancelled &&
              calls == 1, "cancellation is checked between elements");
    }
    {
        Runtime pool({.workers = 1, .capacity = 4});
        Concurrent jobs(pool);
        std::latch entered(1), checkpoint(1);
        auto paused = jobs.run_with_promise<int>([&](Promise<int>& promise) {
            entered.count_down(); checkpoint.wait();
            if (promise.checkpoint()) (void)promise.add_result(1);
        });
        entered.wait(); check(paused.suspend(), "suspend active promise mode"); checkpoint.count_down();
        const auto until = Clock::now() + 5s;
        while (!paused.snapshot().suspended_producers && Clock::now() < until) std::this_thread::yield();
        check(paused.snapshot().suspended_producers == 1 && bool(jobs.close()) && paused.ready() &&
              paused.snapshot().status == FutureStatus::cancelled, "owner removal wakes suspended work");
    }
    {
        Runtime pool({.workers = 2, .capacity = 8});
        Concurrent jobs(pool);
        std::latch entered(1);
        auto running = jobs.run_with_promise<int>([&](Promise<int>& promise) {
            entered.count_down();
            while (!promise.is_cancelled()) std::this_thread::yield();
        });
        entered.wait();
        check(bool(jobs.close()) && running.ready() &&
              running.snapshot().status == FutureStatus::cancelled, "owner removal stops cooperative work");
    }
}
}
int main() {
    try {
        if (!Thread::supported()) return 0;
        computation(); cancellation_and_shutdown();
        std::cout << "concurrent run/map/filter/reduce contracts passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
