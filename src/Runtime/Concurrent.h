#pragma once
#include "Future.h"
#include "TaskScope.h"
#include <algorithm>
#include <tuple>

namespace ii::runtime {
struct ConcurrentOptions final {
    std::size_t grain = 64;
    Priority priority = Priority::normal;
};
namespace concurrent_detail {
template<class T, class Finish> struct Batch final {
    Promise<T> promise;
    Finish finish;
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<core::Error> error;
    std::size_t pending = 0;
    bool sealed = false, finalized = false;

    Batch(std::size_t limit, std::stop_token owner, Finish callback)
        : promise({limit}, owner), finish(std::move(callback)) {}
    void add() { std::lock_guard lock(mutex); ++pending; }
    void fail(core::Error failure) {
        { std::lock_guard lock(mutex); if (!error) error = std::move(failure); }
        (void)promise.future().cancel();
    }
    void await_seal() {
        std::unique_lock lock(mutex); changed.wait(lock, [&] { return sealed; });
    }
    void retire(bool executed) {
        if (!executed && !promise.is_cancelled())
            fail({core::ErrorCode::closed, "Concurrent chunk discarded by executor", {}});
        bool complete;
        {
            std::lock_guard lock(mutex);
            --pending;
            complete = sealed && !pending && !finalized;
            if (complete) finalized = true;
        }
        if (complete) publish();
    }
    void seal() {
        bool complete;
        {
            std::lock_guard lock(mutex);
            sealed = true; complete = !pending && !finalized;
            if (complete) finalized = true;
        }
        changed.notify_all();
        if (complete) publish();
    }
    void publish() {
        try {
            if (const auto admission = promise.future().snapshot().admission; admission != SubmitError::none) {
                (void)promise.reject(admission);
            } else if (error) {
                (void)promise.fail(std::move(*error));
            } else if (promise.is_cancelled()) {
                (void)promise.finish();
            } else {
                auto result = std::invoke(finish, promise);
                if (result) (void)promise.finish();
                else if (result.error().code == core::ErrorCode::cancelled) (void)promise.finish();
                else (void)promise.fail(std::move(result.error()));
            }
        } catch (...) {
            (void)promise.fail({core::ErrorCode::failed, "Concurrent finalizer failed", std::current_exception()});
        }
    }
};
template<class T, class Step, class Finish> struct Chunk final {
    std::shared_ptr<Batch<T, Finish>> batch;
    Step step;
    std::size_t begin, end;
    bool executed = false;
    Chunk(std::shared_ptr<Batch<T, Finish>> owner, Step callback, std::size_t first, std::size_t last)
        : batch(std::move(owner)), step(std::move(callback)), begin(first), end(last) { batch->add(); }
    ~Chunk() { batch->retire(executed); }
    void execute() {
        executed = true;
        (void)batch->promise.start();
        try {
            for (auto i = begin; i < end; ++i) {
                auto ready = batch->promise.checkpoint();
                if (!ready) {
                    if (ready.error().code != core::ErrorCode::cancelled) batch->fail(std::move(ready.error()));
                    break;
                }
                auto result = std::invoke(step, batch->promise, i);
                if (!result) {
                    if (result.error().code != core::ErrorCode::cancelled) batch->fail(std::move(result.error()));
                    break;
                }
                batch->promise.advance_progress();
            }
        } catch (...) {
            batch->fail({core::ErrorCode::failed, "Concurrent element failed", std::current_exception()});
        }
        // Submission never waits for workers. A worker retains its ticket until
        // the caller seals admission, so a successful reducer runs on a worker
        // even if every element completed before submission returned.
        batch->await_seal();
    }
};
template<class T, class Work> struct Run final {
    Promise<T> promise; // Work captures retire before the producer is abandoned.
    Work work;
    Run(PromiseOptions options, std::stop_token owner, Work callback)
        : promise(options, owner), work(std::move(callback)) {}
    void execute() {
        if (!promise.start()) return;
        try {
            auto ready = promise.checkpoint();
            if (!ready) {
                if (ready.error().code == core::ErrorCode::cancelled) (void)promise.finish();
                else (void)promise.fail(std::move(ready.error()));
                return;
            }
            std::invoke(work, promise);
            (void)promise.finish();
        } catch (...) {
            (void)promise.fail({core::ErrorCode::failed, "Concurrent work threw", std::current_exception()});
        }
    }
};
}

// Instance-owned work on existing Runtime workers. The runtime outlives this
// owner. close requests cooperative stop, then awaits TaskScope retirement.
class Concurrent final {
public:
    explicit Concurrent(Runtime& runtime) : runtime_(runtime) {}
    ~Concurrent() { (void)close(); }
    Concurrent(const Concurrent&) = delete;
    Concurrent& operator=(const Concurrent&) = delete;
    Concurrent(Concurrent&&) = delete;
    Concurrent& operator=(Concurrent&&) = delete;

    template<class Work, class... Args>
    [[nodiscard]] auto run(Work work, Args&&... args) {
        return run_with_priority(Priority::normal, std::move(work), std::forward<Args>(args)...);
    }
    template<class Work, class... Args>
    [[nodiscard]] auto run_with_priority(Priority priority, Work work, Args&&... args) {
        using Out = std::remove_cvref_t<std::invoke_result_t<Work&, std::decay_t<Args>...>>;
        return run_with_promise<Out>(
            [work = std::move(work), arguments = std::make_tuple(std::forward<Args>(args)...)](Promise<Out>& promise) mutable {
                auto invoke = [&](auto&&... values) -> decltype(auto) {
                    return std::invoke(work, std::forward<decltype(values)>(values)...);
                };
                if constexpr (std::is_void_v<Out>) std::apply(invoke, std::move(arguments));
                else {
                    auto result = promise.add_result(std::apply(invoke, std::move(arguments)));
                    if (!result && result.error().code != core::ErrorCode::cancelled)
                        (void)promise.fail(std::move(result.error()));
                }
            }, {.max_results = 1}, priority);
    }
    template<class T, class Work> requires std::invocable<Work&, Promise<T>&>
    [[nodiscard]] Future<T> run_with_promise(Work work, PromiseOptions options = {}, Priority priority = Priority::normal) {
        auto job = std::make_shared<concurrent_detail::Run<T, Work>>(options, stop_.get_token(), std::move(work));
        auto future = job->promise.future();
        try {
            const auto submitted = lifetime_.post(runtime_, [job] { job->execute(); }, priority);
            job->promise.admitted(submitted);
            if (!submitted) (void)job->promise.reject(submitted.error);
        } catch (...) {
            (void)job->promise.fail({core::ErrorCode::failed, "Concurrent submission threw", std::current_exception()});
        }
        return future;
    }
    template<class In, class Map>
    [[nodiscard]] auto mapped(std::vector<In> input, Map map, ConcurrentOptions options = {}) {
        using Out = std::remove_cvref_t<std::invoke_result_t<Map&, const In&>>;
        static_assert(!std::is_void_v<Out>, "mapped requires a value; use map for mutating callbacks");
        auto data = std::make_shared<std::vector<In>>(std::move(input));
        return schedule<Out>(data->size(), data->size(),
            [data, map = std::move(map)](Promise<Out>& promise, std::size_t i) mutable {
                return promise.add_result(std::invoke(map, std::as_const(*data)[i]), i);
            }, [](Promise<Out>&) -> core::Result<void> { return {}; }, options);
    }
    template<class In, class Keep>
    [[nodiscard]] Future<In> filtered(std::vector<In> input, Keep keep, ConcurrentOptions options = {}) {
        auto data = std::make_shared<std::vector<In>>(std::move(input));
        return schedule<In>(data->size(), data->size(),
            [data, keep = std::move(keep)](Promise<In>& promise, std::size_t i) mutable -> core::Result<void> {
                if (std::invoke(keep, std::as_const(*data)[i])) return promise.add_result(std::move((*data)[i]), i);
                return {};
            }, [](Promise<In>&) -> core::Result<void> { return {}; }, options);
    }
    template<class In, class Map>
    [[nodiscard]] Future<std::vector<In>> map(std::vector<In> input, Map map, ConcurrentOptions options = {}) {
        static_assert(!std::same_as<In, bool>, "vector<bool> packs shared bits; use mapped instead");
        auto data = std::make_shared<std::vector<In>>(std::move(input));
        return schedule<std::vector<In>>(data->size(), 1,
            [data, map = std::move(map)](auto&, std::size_t i) mutable -> core::Result<void> {
                std::invoke(map, (*data)[i]); return {};
            }, [data](auto& promise) { return promise.add_result(std::move(*data)); }, options);
    }
    template<class In, class Keep>
    [[nodiscard]] Future<std::vector<In>> filter(std::vector<In> input, Keep keep, ConcurrentOptions options = {}) {
        auto data = std::make_shared<std::vector<In>>(std::move(input));
        auto selected = std::make_shared<std::vector<std::uint8_t>>(data->size());
        return schedule<std::vector<In>>(data->size(), 1,
            [data, selected, keep = std::move(keep)](auto&, std::size_t i) mutable -> core::Result<void> {
                (*selected)[i] = bool(std::invoke(keep, std::as_const(*data)[i])); return {};
            }, [data, selected](auto& promise) {
                std::vector<In> result; result.reserve(data->size());
                for (std::size_t i = 0; i < data->size(); ++i)
                    if ((*selected)[i]) result.push_back(std::move((*data)[i]));
                return promise.add_result(std::move(result));
            }, options);
    }
    template<class In, class Map, class Reduce, class Result>
    [[nodiscard]] Future<Result> mapped_reduced(std::vector<In> input, Map map, Reduce reduce,
                                                Result initial, ConcurrentOptions options = {}) {
        using Out = std::remove_cvref_t<std::invoke_result_t<Map&, const In&>>;
        auto data = std::make_shared<std::vector<In>>(std::move(input));
        auto values = std::make_shared<std::vector<std::optional<Out>>>(data->size());
        return schedule<Result>(data->size(), 1,
            [data, values, map = std::move(map)](auto&, std::size_t i) mutable -> core::Result<void> {
                (*values)[i].emplace(std::invoke(map, std::as_const(*data)[i])); return {};
            }, [values, reduce = std::move(reduce), initial = std::move(initial)](auto& promise) mutable -> core::Result<void> {
                for (auto& value : *values) {
                    auto ready = promise.checkpoint(); if (!ready) return ready;
                    std::invoke(reduce, initial, std::move(value.value()));
                }
                return promise.add_result(std::move(initial));
            }, options);
    }
    template<class In, class Keep, class Reduce, class Result>
    [[nodiscard]] Future<Result> filtered_reduced(std::vector<In> input, Keep keep, Reduce reduce,
                                                  Result initial, ConcurrentOptions options = {}) {
        auto data = std::make_shared<std::vector<In>>(std::move(input));
        auto selected = std::make_shared<std::vector<std::uint8_t>>(data->size());
        return schedule<Result>(data->size(), 1,
            [data, selected, keep = std::move(keep)](auto&, std::size_t i) mutable -> core::Result<void> {
                (*selected)[i] = bool(std::invoke(keep, std::as_const(*data)[i])); return {};
            }, [data, selected, reduce = std::move(reduce), initial = std::move(initial)](auto& promise) mutable -> core::Result<void> {
                for (std::size_t i = 0; i < data->size(); ++i) if ((*selected)[i]) {
                    auto ready = promise.checkpoint(); if (!ready) return ready;
                    std::invoke(reduce, initial, std::move((*data)[i]));
                }
                return promise.add_result(std::move(initial));
            }, options);
    }
    template<class In, class Map>
        requires std::copy_constructible<std::remove_cvref_t<std::invoke_result_t<Map&, const In&>>>
    [[nodiscard]] auto blocking_mapped(std::vector<In> input, Map map, ConcurrentOptions options = {})
        -> core::Result<std::vector<std::remove_cvref_t<std::invoke_result_t<Map&, const In&>>>> {
        if (future_detail::blocking_forbidden()) return std::unexpected(future_detail::wait_error(FutureWait::would_block));
        return mapped(std::move(input), std::move(map), options).get_all();
    }
    template<class In, class Keep> requires std::copy_constructible<In>
    [[nodiscard]] core::Result<std::vector<In>> blocking_filtered(std::vector<In> input, Keep keep, ConcurrentOptions options = {}) {
        if (future_detail::blocking_forbidden()) return std::unexpected(future_detail::wait_error(FutureWait::would_block));
        return filtered(std::move(input), std::move(keep), options).get_all();
    }
    template<class In, class Map> requires std::copy_constructible<In>
    [[nodiscard]] core::Result<std::vector<In>> blocking_map(std::vector<In> input, Map map, ConcurrentOptions options = {}) {
        if (future_detail::blocking_forbidden()) return std::unexpected(future_detail::wait_error(FutureWait::would_block));
        return this->map(std::move(input), std::move(map), options).get();
    }
    template<class In, class Keep> requires std::copy_constructible<In>
    [[nodiscard]] core::Result<std::vector<In>> blocking_filter(std::vector<In> input, Keep keep, ConcurrentOptions options = {}) {
        if (future_detail::blocking_forbidden()) return std::unexpected(future_detail::wait_error(FutureWait::would_block));
        return filter(std::move(input), std::move(keep), options).get();
    }
    template<class In, class Map, class Reduce, class Result> requires std::copy_constructible<Result>
    [[nodiscard]] core::Result<Result> blocking_mapped_reduced(std::vector<In> input, Map map, Reduce reduce, Result initial, ConcurrentOptions options = {}) {
        if (future_detail::blocking_forbidden()) return std::unexpected(future_detail::wait_error(FutureWait::would_block));
        return mapped_reduced(std::move(input), std::move(map), std::move(reduce), std::move(initial), options).get();
    }
    template<class In, class Keep, class Reduce, class Result> requires std::copy_constructible<Result>
    [[nodiscard]] core::Result<Result> blocking_filtered_reduced(std::vector<In> input, Keep keep, Reduce reduce, Result initial, ConcurrentOptions options = {}) {
        if (future_detail::blocking_forbidden()) return std::unexpected(future_detail::wait_error(FutureWait::would_block));
        return filtered_reduced(std::move(input), std::move(keep), std::move(reduce), std::move(initial), options).get();
    }
    [[nodiscard]] ThreadResult close() {
        if (lifetime_.is_current()) return std::unexpected(ThreadError{ThreadErrorCode::self_join, {}});
        stop_.request_stop();
        return lifetime_.close();
    }
private:
    template<class Out, class Step, class Finish>
    Future<Out> schedule(std::size_t count, std::size_t limit, Step step, Finish finish, ConcurrentOptions options) {
        static_assert(std::copy_constructible<Step>, "Parallel callbacks need an independent copy per chunk");
        auto batch = std::make_shared<concurrent_detail::Batch<Out, Finish>>(limit, stop_.get_token(), std::move(finish));
        auto future = batch->promise.future();
        (void)batch->promise.set_progress_range(0, count);
        if (!options.grain) {
            batch->fail({core::ErrorCode::invalid_argument, "Concurrent grain must be positive", {}});
        } else if (lifetime_.closed()) {
            (void)batch->promise.reject(SubmitError::stopped);
        } else {
            try {
                const auto capacity = runtime_.stats().capacity;
                const auto grain = std::max(options.grain, count / capacity + (count % capacity != 0));
                for (std::size_t begin = 0; begin < count;) {
                    const auto end = begin + std::min(grain, count - begin);
                    auto chunk = std::make_shared<concurrent_detail::Chunk<Out, Step, Finish>>(batch, step, begin, end);
                    const auto submitted = lifetime_.post(runtime_, [chunk] { chunk->execute(); }, options.priority);
                    batch->promise.admitted(submitted);
                    if (!submitted) {
                        batch->fail({submitted.error == SubmitError::full ? core::ErrorCode::full : core::ErrorCode::closed,
                                     "Concurrent batch admission rejected", {}});
                        break;
                    }
                    begin = end;
                }
            } catch (...) {
                batch->fail({core::ErrorCode::failed, "Concurrent batch submission threw", std::current_exception()});
            }
        }
        batch->seal();
        return future;
    }
    Runtime& runtime_;
    std::stop_source stop_;
    TaskScope lifetime_;
};

template<class Work, class... Args> class TaskBuilder final {
public:
    TaskBuilder(Work work, std::tuple<Args...> arguments = {}, Priority priority = Priority::normal)
        : work_(std::move(work)), arguments_(std::move(arguments)), priority_(priority) {}
    template<class... Values> requires (sizeof...(Args) == 0)
    [[nodiscard]] auto with_arguments(Values&&... values) && {
        return TaskBuilder<Work, std::decay_t<Values>...>(std::move(work_),
            std::make_tuple(std::forward<Values>(values)...), priority_);
    }
    TaskBuilder& with_priority(Priority priority) & { priority_ = priority; return *this; }
    TaskBuilder&& with_priority(Priority priority) && { priority_ = priority; return std::move(*this); }
    [[nodiscard]] auto spawn(Concurrent& concurrent) && {
        return std::apply([&](auto&&... args) {
            return concurrent.run_with_priority(priority_, std::move(work_), std::forward<decltype(args)>(args)...);
        }, std::move(arguments_));
    }
private:
    Work work_;
    std::tuple<Args...> arguments_;
    Priority priority_;
};
template<class Work> [[nodiscard]] auto task(Work work) { return TaskBuilder<Work>(std::move(work)); }
} // namespace ii::runtime
