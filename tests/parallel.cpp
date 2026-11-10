#include <iiSDKCore/Runtime/Runtime.h>
#include <iiSDKCore/Runtime/ObjectProperties.h>
#include <atomic>
#include <array>
#include <algorithm>
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

void distributed_queues() {
    Runtime runtime({.workers=2,.capacity=12,.queues=4});
    Gate gate;
    std::atomic<int> finished=0;
    const auto first=runtime.post([&]{gate.enter(); ++finished;});
    const auto second=runtime.post([&]{gate.enter(); ++finished;});
    const bool both=gate.wait(2);
    bool accepted=first && second;
    for(int i=0;i<4;++i) accepted=bool(runtime.post([&]{++finished;})) && accepted;
    const auto snapshot=runtime.queue_stats();
    std::size_t capacity=0,pending=0,active=0,ready=0,used=0;
    for(const auto& queue:snapshot) {
        capacity+=queue.capacity; pending+=queue.pending; active+=queue.active; ready+=queue.ready;
        used+=queue.pending!=0;
    }
    const auto queues=runtime.stats().queues;
    gate.release();
    const auto drained=runtime.wait_idle();
    const auto settled=runtime.queue_stats();
    std::uint64_t completed=0;
    for(const auto& queue:settled) completed+=queue.executed;
    check(accepted && both && queues==4 && snapshot.size()==4 && capacity==12 &&
          pending==6 && active==2 && ready==4 && used>=2,
          "independent tasks occupy multiple queues and execute asynchronously in parallel");
    check(bool(drained) && finished==6 && completed==6 &&
          std::ranges::all_of(settled,[](const auto& item){return item.pending==0 && item.active==0 && item.ready==0;}),
          "all distributed work settles and returns queue capacity");
}

void hardware_worker_policy() {
    const auto hint=std::thread::hardware_concurrency();
    const auto expected=(std::min)(std::size_t{8},hint ? static_cast<std::size_t>(hint) : std::size_t{1});
    Runtime bounded_auto({.capacity=1,.worker_policy=RuntimeWorkerPolicy::bounded});
    check(bounded_auto.stats().workers==1,"explicit bounded policy respects configured capacity");
    Runtime automatic({.capacity=8,.worker_policy=RuntimeWorkerPolicy::hardware});
    check(automatic.stats().workers==expected && automatic.stats().queues==expected,
          "hardware policy uses the reported logical concurrency within the slot bound");
    Runtime explicit_count({.workers=1,.capacity=4,.worker_policy=RuntimeWorkerPolicy::hardware});
    check(explicit_count.stats().workers==1,"explicit worker count overrides automatic sizing");
    bool invalid=false;
    try { Runtime rejected({.capacity=1,.worker_policy=static_cast<RuntimeWorkerPolicy>(255)}); }
    catch(const std::invalid_argument&) { invalid=true; }
    check(invalid,"unknown worker policy is rejected before workers start");
}

void queue_sharing_and_serial_order() {
    Runtime runtime({.workers=1,.capacity=8,.queues=3});
    SerialQueue serial(runtime);
    Gate gate;
    std::vector<int> order;
    const auto blocker=serial.post([&]{gate.enter();});
    const bool entered=gate.wait(1);
    const auto older=serial.post([&]{order.push_back(1);});
    const auto ordinary=runtime.post([&]{order.push_back(2);});
    const auto later=serial.post([&]{order.push_back(3);});
    gate.release();
    const auto drained=runtime.wait_idle();
    const auto snapshot=runtime.queue_stats();
    std::uint64_t shared=0;
    for(const auto& queue:snapshot) shared+=queue.cross_queue;
    check(blocker && entered && older && ordinary && later && drained &&
          order==std::vector<int>({1,2,3}),
          "distributed selection preserves oldest-ready order and object FIFO");
    check(shared>0,"a worker can execute work assigned to a different queue");
    bool rejected=false;
    try { Runtime invalid({.workers=1,.capacity=2,.queues=3}); }
    catch(const std::invalid_argument&) { rejected=true; }
    check(rejected,"queue count cannot exceed bounded slot capacity");
}

void serial_drain_and_token_waiters() {
    Runtime runtime({.workers=4,.capacity=48});
    SerialQueue serial(runtime);
    Gate gate;
    std::vector<int> order;
    check(bool(serial.post([&]{gate.enter();})),"start the serial lane");
    check(gate.wait(1),"serial lane reached the gate");
    std::vector<TaskToken> tokens;
    for(int i=0;i<32;++i) {
        const auto item=serial.post([&,i]{order.push_back(i);});
        check(bool(item),"queue serial work during an active operation");
        tokens.push_back(item.token);
    }
    std::atomic<int> waited=0;
    std::array<std::thread,3> observers;
    for(std::size_t i=0;i<observers.size();++i)
        observers[i]=std::thread([&,i] { if(tokens[10*i].wait()) ++waited; });
    runtime.request_stop(ShutdownMode::drain);
    gate.release();
    const auto result=runtime.join();
    for(auto& observer:observers) observer.join();
    bool ordered=order.size()==tokens.size();
    for(std::size_t i=0;i<order.size();++i) ordered=ordered && order[i]==static_cast<int>(i);
    check(result && ordered && waited==3 && runtime.stats().pending==0,
          "draining wakes eligible serial work and all token waiters");
}

void priority_and_ready_cancellation() {
    {
        Runtime runtime({.workers=1,.capacity=24,.queues=4});
        Gate gate;
        std::vector<int> order;
        const auto blocker=runtime.post([&]{gate.enter();},Priority::background);
        const bool entered=gate.wait(1);
        bool accepted=bool(blocker);
        for(int i=0;i<6;++i) accepted=bool(runtime.post([&,i]{order.push_back(100+i);},Priority::high)) && accepted;
        for(int i=0;i<4;++i) accepted=bool(runtime.post([&,i]{order.push_back(200+i);},Priority::normal)) && accepted;
        for(int i=0;i<2;++i) accepted=bool(runtime.post([&,i]{order.push_back(300+i);},Priority::background)) && accepted;
        gate.release();
        const auto done=runtime.wait_idle();
        const std::vector<int> expected{100,101,102,103,200,201,300};
        check(entered && accepted && done && order.size()==12 &&
              std::equal(expected.begin(),expected.end(),order.begin()),
              "weighted priority fairness and FIFO hold across all queues");
    }
    {
        Runtime runtime({.workers=1,.capacity=32,.queues=4});
        SerialQueue serial(runtime);
        Gate gate;
        std::vector<int> order;
        const auto blocker=runtime.post([&]{gate.enter();});
        const bool entered=gate.wait(1);
        bool accepted=bool(blocker);
        std::vector<TaskToken> tokens;
        for(int i=0;i<24;++i) {
            const auto task=runtime.post([&,i]{order.push_back(i);});
            accepted=bool(task) && accepted;
            tokens.push_back(task.token);
        }
        const auto head=serial.post([&]{order.push_back(100);});
        const auto next=serial.post([&]{order.push_back(101);});
        bool cancelled=runtime.cancel(head.token);
        for(const int index:{0,3,7,11,23}) cancelled=runtime.cancel(tokens[static_cast<std::size_t>(index)]) && cancelled;
        gate.release();
        const auto done=runtime.wait_idle();
        std::vector<int> expected;
        for(int i=0;i<24;++i) if(i!=0 && i!=3 && i!=7 && i!=11 && i!=23) expected.push_back(i);
        expected.push_back(101);
        const auto snapshot=runtime.queue_stats();
        std::uint64_t cancelled_count=0;
        for(const auto& queue:snapshot) cancelled_count+=queue.cancelled;
        check(entered && accepted && head && next && cancelled && done && order==expected && cancelled_count==6,
              "removing arbitrary ready jobs and a serial head preserves remaining order");
    }
    Runtime tiny({.workers=4,.capacity=2});
    check(tiny.stats().queues==2,"automatic queue count stays within slot capacity");
}

void distributed_scheduler_model() {
    Runtime runtime({.workers=1,.capacity=192,.queues=7});
    std::array<std::unique_ptr<SerialQueue>,3> lanes;
    for(auto& lane:lanes) lane=std::make_unique<SerialQueue>(runtime);
    Gate gate;
    std::vector<int> actual;
    const auto blocker=runtime.post([&]{gate.enter();},Priority::background);
    const bool entered=gate.wait(1);
    struct Item final { TaskToken token; Priority priority; int lane; bool removed=false,finished=false; };
    std::vector<Item> items;
    bool accepted=bool(blocker),cancelled=true;
    for(int i=0;i<150;++i) {
        const auto priority=static_cast<Priority>((i*5+1)%3);
        const int lane=i%4==0 ? -1 : (i*7)%3;
        auto task=[&,i]{actual.push_back(i);};
        const auto submitted=lane<0 ? runtime.post(task,priority) : lanes[static_cast<std::size_t>(lane)]->post(task,priority);
        accepted=bool(submitted) && accepted;
        items.push_back({submitted.token,priority,lane});
    }
    for(std::size_t i=0;i<items.size();++i) if(i%7==0 || i%23==0) {
        cancelled=runtime.cancel(items[i].token) && cancelled;
        items[i].removed=true;
    }
    constexpr std::array wheel{Priority::high,Priority::high,Priority::high,Priority::high,
                               Priority::normal,Priority::normal,Priority::background};
    std::size_t cursor=0;
    std::vector<int> expected;
    const auto remaining=std::ranges::count_if(items,[](const auto& item){return !item.removed;});
    while(expected.size()<static_cast<std::size_t>(remaining)) {
        std::size_t chosen=items.size();
        for(std::size_t attempt=0;attempt<wheel.size() && chosen==items.size();++attempt) {
            const auto priority=wheel[cursor]; cursor=(cursor+1)%wheel.size();
            for(std::size_t i=0;i<items.size();++i) {
                const auto& item=items[i];
                if(item.removed || item.finished || item.priority!=priority) continue;
                bool blocked=false;
                if(item.lane>=0) for(std::size_t j=0;j<i;++j)
                    if(items[j].lane==item.lane && !items[j].removed && !items[j].finished) { blocked=true; break; }
                if(!blocked) { chosen=i; break; }
            }
        }
        if(chosen==items.size()) break;
        items[chosen].finished=true;
        expected.push_back(static_cast<int>(chosen));
    }
    gate.release();
    const auto done=runtime.wait_idle();
    check(entered && accepted && cancelled && done && expected.size()==static_cast<std::size_t>(remaining) && actual==expected,
          "distributed priority queues match the FIFO and cancellation scheduling model");
}

int main() {
    try {
        if (!Thread::supported()) {
            Runtime dormant;
            check(dormant.stats().workers==0 && dormant.post([]{}).error==SubmitError::overloaded,
                  "threadless target retains a dormant base runtime");
            bool rejected = false;
            try { Runtime runtime({.workers=1}); } catch (const std::exception&) { rejected = true; }
            check(rejected, "parallel runtime never silently falls back to inline execution");
            std::cout << "parallel runtime unsupported contract passed\n";
            return 0;
        }
        distributed_queues();
        hardware_worker_policy();
        queue_sharing_and_serial_order();
        serial_drain_and_token_waiters();
        priority_and_ready_cancellation();
        distributed_scheduler_model();
        Runtime runtime({.workers=2});
        check(runtime.stats().workers == 2, "explicit runtime owns the requested workers");
        Gate parallel;
        const auto caller = Thread::current_id();
        std::atomic<int> background = 0;
        for (int i = 0; i < 2; ++i) check(bool(runtime.post([&] {
            if (Thread::current_id() != caller) ++background;
            parallel.enter();
        })), "submit parallel work");
        const bool overlap = parallel.wait(2);
        parallel.release();
        check(bool(runtime.wait_idle()) && overlap && background == 2, "allocated workers run tasks concurrently off caller");

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
