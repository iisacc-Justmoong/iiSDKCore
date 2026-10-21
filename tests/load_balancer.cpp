#include <iiSDKCore/Runtime/LoadBalancer.h>
#include <iiSDKCore/Runtime/EventLoop.h>
#include <iiSDKCore/Runtime/Query.h>
#include <iiSDKCore/Runtime/Operation.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace ii::runtime;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Gate final {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered=false, released=false;
    void enter() {
        std::unique_lock lock(mutex); entered=true; changed.notify_all();
        if (!changed.wait_for(lock,5s,[&]{return released;})) throw std::runtime_error("gate timeout");
    }
    void wait() {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock,5s,[&]{return entered;})) throw std::runtime_error("start timeout");
    }
    void release() { std::lock_guard lock(mutex); released=true; changed.notify_all(); }
};

void routing_and_limits() {
    EventLoop small(16), large(16);
    LoadBalancer balancer({.max_targets=2,.capacity=5});
    const auto a=balancer.add_target(small,{.name="small",.capacity=2}).value();
    const auto b=balancer.add_target(large,{.name="large",.capacity=4}).value();
    check(!balancer.add_target(small),"target table is bounded");
    for (int i=0;i<5;++i) check(bool(balancer.post([]{})),"bounded admission");
    check(balancer.target_stats(a)->in_flight==2 && balancer.target_stats(b)->in_flight==3,
          "capacity-normalized load routes across heterogeneous targets");
    check(balancer.post([]{}).error==SubmitError::full,"global limit applies before a target fills");
    check(small.poll({16,1s})==2 && large.poll({16,1s})==3,"selected native executors run their own jobs");
    check(bool(balancer.wait_idle()) && balancer.stats().completed==5,"completion accounting");
    check(bool(balancer.remove_target(a)) && !balancer.target_stats(a),"remove registered target");
    const auto replacement=balancer.add_target(small,{.capacity=2}).value();
    check(replacement!=a && !balancer.report_health(a,true),"reused slots reject stale target identities");
    LoadBalancer foreign;
    check(!foreign.report_health(b,true),"identities cannot cross balancers");
    check(balancer.post({}).error==SubmitError::invalid,"empty work rejected");
    check(balancer.submit([]{}, {.units=0}).error==SubmitError::invalid,"zero cost rejected");
    check(balancer.post([]{},static_cast<Priority>(90)).error==SubmitError::invalid,"invalid priority rejected");
    check(!balancer.add_target(small,{.capacity=0}),"invalid target capacity rejected");
}

void health_and_resource_selection() {
    EventLoop first(16), second(16);
    LoadBalancer balancer({.policy=BalancePolicy::round_robin});
    const auto a=balancer.add_target(first,{.capacity=8,.memory_capacity=64,.capabilities=1,
        .unhealthy_threshold=2,.healthy_threshold=2,.require_health_check=true}).value();
    const auto b=balancer.add_target(second,{.capacity=8,.memory_capacity=128,.capabilities=3}).value();
    check(balancer.target_stats(a)->health==TargetHealth::initial,"health-gated targets start excluded");
    check(balancer.report_health(a,true) && balancer.target_stats(a)->health==TargetHealth::initial,"recovery threshold");
    check(balancer.report_health(a,true) && balancer.target_stats(a)->health==TargetHealth::healthy,"initial checks admit target");
    check(bool(balancer.submit([]{}, {.units=3,.memory_bytes=64,.capabilities=1})),"resource reservation");
    check(bool(balancer.submit([]{}, {.units=2,.memory_bytes=100,.capabilities=2})),"capability selection");
    check(balancer.submit([]{}, {.memory_bytes=1,.capabilities=4}).error==SubmitError::overloaded,"missing capability rejected");
    check(balancer.submit([]{}, {.memory_bytes=65,.capabilities=1}).error==SubmitError::overloaded,"memory budget cannot overcommit");
    check(balancer.target_stats(a)->memory_bytes==64 && balancer.target_stats(b)->units==2,"reservation snapshot");
    check(balancer.report_health(a,false) && balancer.target_stats(a)->health==TargetHealth::healthy,"single failure is tolerated");
    check(balancer.report_health(a,false) && balancer.target_stats(a)->health==TargetHealth::unhealthy,"failure threshold excludes");
    check(balancer.report_health(b,false) && balancer.report_health(b,false) && balancer.report_health(b,false),"second target unhealthy");
    check(balancer.post([]{}).error==SubmitError::overloaded,"all-unhealthy rejects work without fail-open");
    check(first.poll({16,1s})==1 && second.poll({16,1s})==1,"health change preserves accepted work");
    check(balancer.report_health(a,true) && balancer.report_health(a,true),"target recovers");
    check(bool(balancer.post([]{})),"recovered target reused");
    check(balancer.request_drain(a) && balancer.post([]{}).error==SubmitError::overloaded,"draining rejects new work");
    check(first.poll({16,1s})==1 && bool(balancer.remove_target(a,ShutdownMode::drain)),"drain finishes accepted work");
}

struct RetiringCapture final {
    LoadBalancer& balancer;
    bool& held;
    bool& self_join;
    RetiringCapture(LoadBalancer& owner,bool& capacity_held,bool& detected)
        :balancer(owner),held(capacity_held),self_join(detected) {}
    ~RetiringCapture() {
        held=balancer.post([]{}).error==SubmitError::full;
        const auto result=balancer.wait_idle();
        self_join=!result && result.error().code==ThreadErrorCode::self_join;
    }
};
void cancellation_and_retirement() {
    EventLoop loop(8);
    LoadBalancer balancer({.capacity=1});
    const auto target=balancer.add_target(loop,{.capacity=1}).value();
    bool held=false,self_join=false,ran=false;
    auto capture=std::make_shared<RetiringCapture>(balancer,held,self_join);
    const auto work=balancer.post([capture,&ran]{ran=true;}); capture.reset();
    check(bool(work) && work.token.cancel(),"backend cancellation token is preserved");
    check(held && self_join && !ran,"capture is destroyed before quota returns and self-wait cannot deadlock");
    check(balancer.stats().in_flight==0 && balancer.stats().cancelled==1,"cancelled work releases quota exactly once");
    auto resource=std::make_shared<int>(1); std::weak_ptr<int> weak=resource;
    check(bool(balancer.post([resource]{})),"removal cleanup admitted"); resource.reset();
    check(bool(balancer.remove_target(target)) && weak.expired() && loop.stats().pending==0,"removal cancels owned work only");
    check(balancer.stats().cancelled==2,"removal cancellation accounted");
}

struct RejectingExecutor final {
    bool throws=false;
    Submission post(Task,Priority) {
        if (throws) throw std::runtime_error("submission failure");
        return {{},SubmitError::full};
    }
};
struct InlineExecutor final {
    Submission post(Task task,Priority) { task(); return {}; }
};
void rejection_inline_and_failure() {
    RejectingExecutor rejected;
    LoadBalancer balancer({.capacity=1});
    const auto target=balancer.add_target(rejected).value();
    int ran=0;
    check(balancer.post([&]{++ran;}).error==SubmitError::full,"backend rejection propagated");
    check(!ran && !balancer.stats().in_flight && balancer.stats().rejected==1,"rejection rolls back reservations");
    rejected.throws=true;
    bool threw=false;
    try { (void)balancer.post([&]{++ran;}); } catch(const std::runtime_error&) { threw=true; }
    check(threw,"submission exception propagates");
    check(!ran && !balancer.stats().in_flight && balancer.stats().rejected==2,"throwing admission releases owned work");
    check(bool(balancer.remove_target(target)),"failed backend removable");
    InlineExecutor inline_executor;
    (void)balancer.add_target(inline_executor).value();
    check(bool(balancer.post([&] {
        ++ran;
        check(balancer.post([]{}).error==SubmitError::full,"inline nested submission obeys held capacity");
    })) && ran==1 && balancer.stats().completed==1,"inline completion is reentrant-safe");
    threw=false;
    try { (void)balancer.post([]{throw std::runtime_error("inline failure");}); } catch(const std::runtime_error&) { threw=true; }
    check(threw && balancer.stats().failed==1 && balancer.stats().rejected==3,"already executed inline failure is not an admission rejection");
    const auto inline_closed=balancer.close();
    check(!inline_closed && inline_closed.error().code==ThreadErrorCode::work_failed,"inline failure survives cleanup");
    check(balancer.post([]{}).error==SubmitError::stopped,"closed balancer rejects work");

    Runtime runtime({.workers=2,.capacity=8});
    LoadBalancer failures;
    (void)failures.add_target(runtime,{.capacity=8}).value();
    check(bool(failures.post([]{throw std::runtime_error("work failure");})),"failing task accepted");
    const auto failed=failures.wait_idle();
    check(!failed && failed.error().code==ThreadErrorCode::work_failed && failures.stats().failed==1,"task failure remains observable");
    check(!runtime.wait_idle(),"native executor also observes work failure");
}

void operation_and_shutdown() {
    EventLoop loop(8);
    LoadBalancer compute;
    const auto target=compute.add_target(loop,{.capacity=8}).value();
    OperationOwner owner;
    auto queued=owner.submit<int>(compute,[](WorkContext&) -> ii::core::Result<int>{return 42;});
    check(queued.admission()==SubmitError::none && queued.cancel(),"Operation cancellation crosses the balancer unchanged");
    check(queued.result()->status==OperationStatus::cancelled && compute.stats().cancelled==1,"Operation settles on discard");
    auto successful=owner.submit<int>(compute,[](WorkContext&) -> ii::core::Result<int>{return 42;});
    check(loop.poll()==1 && successful.result()->value==42,"Operation executes through the common API");
    auto payload=std::make_shared<int>(9); std::weak_ptr<int> weak=payload;
    check(bool(compute.post([payload]{})),"executor shutdown payload"); payload.reset();
    loop.request_stop();
    check(weak.expired() && compute.stats().cancelled==2,"executor shutdown returns reservations");
    check(bool(compute.remove_target(target)),"stopped executor can be removed safely");

    EventLoop bounded(4);
    LoadBalancer large;
    const auto id=large.add_target(bounded,{.capacity=std::numeric_limits<std::size_t>::max()}).value();
    check(bool(large.submit([]{}, {.units=std::numeric_limits<std::size_t>::max(),
        .memory_bytes=std::numeric_limits<std::size_t>::max()})),"maximum integer reservation is representable");
    check(large.post([]{}).error==SubmitError::overloaded,"unit reservation cannot overflow");
    check(bounded.poll()==1 && large.target_stats(id)->units==0,"maximum reservation retires exactly");
    check(bool(large.submit([]{}, {.memory_bytes=std::numeric_limits<std::size_t>::max()})),"unlimited memory is accounted");
    check(large.submit([]{}, {.memory_bytes=1}).error==SubmitError::overloaded,"unlimited memory counter still cannot overflow");
}

void active_removal() {
    Runtime runtime({.workers=1,.capacity=8});
    LoadBalancer compute;
    const auto target=compute.add_target(runtime,{.capacity=8}).value();
    Gate active;
    std::atomic<bool> self_close=false,foreign=false,removed=false,remove_ok=false;
    check(bool(compute.post([&] {
        const auto closed=compute.close();
        self_close=!closed && closed.error().code==ThreadErrorCode::self_join;
        active.enter();
    })),"active removal work"); active.wait();
    auto resource=std::make_shared<int>(1); std::weak_ptr<int> weak=resource;
    check(bool(compute.post([resource]{})),"owned queued resource"); resource.reset();
    check(bool(runtime.post([&]{foreign=true;})),"unrelated executor job");
    Thread remover;
    check(bool(remover.start([&](ThreadContext&) {
        remove_ok=bool(compute.remove_target(target)); removed=true;
    })),"removal thread starts");
    const auto deadline=Clock::now()+5s;
    while(!weak.expired() && Clock::now()<deadline) std::this_thread::yield();
    const bool cancelled=weak.expired();
    const bool waited=!removed;
    const auto busy=compute.close();
    active.release(); check(bool(remover.join()),"removal thread joins");
    check(cancelled && waited && self_close,"removal cancels queued captures and waits for active work");
    check(!busy && busy.error().code==ThreadErrorCode::busy,"concurrent lifecycle operations are rejected");
    check(remove_ok && bool(runtime.wait_idle()) && foreign,"removal leaves unrelated executor work intact");
}

struct DelayedAdmission final {
    EventLoop& loop;
    Gate& admitted;
    Submission post(Task task,Priority priority) {
        const auto result=loop.post(std::move(task),priority);
        admitted.enter();
        return result;
    }
};
void request_close_races_registration() {
    EventLoop loop(4);
    Gate admitted;
    DelayedAdmission executor{loop,admitted};
    TaskScope scope;
    auto resource=std::make_shared<int>(3); std::weak_ptr<int> weak=resource;
    Thread submitter;
    check(bool(submitter.start([&,payload=std::move(resource)](ThreadContext&) {
        check(bool(scope.post(executor,[payload]{})),"delayed admission was accepted before close");
    })),"admission thread starts");
    admitted.wait();
    scope.request_close(); // the executor has not returned its token yet
    const bool closed=scope.closed() && scope.post(loop,[]{}).error==SubmitError::stopped;
    admitted.release(); check(bool(submitter.join()),"admission thread joins");
    check(closed && weak.expired() && loop.stats().pending==0,"late token registration completes requested cancellation");
    check(bool(scope.close()),"requested scope close retains its final lifetime barrier");
}

void cancel_all_before_waiting() {
    Runtime first({.workers=1,.capacity=4}),second({.workers=1,.capacity=4});
    LoadBalancer compute;
    const auto a=compute.add_target(first,{.capacity=4}).value();
    (void)compute.add_target(second,{.capacity=4}).value();
    Gate one,two;
    check(bool(compute.post([&]{one.enter();})),"first target active"); one.wait();
    check(compute.request_drain(a),"isolate second target for the next job");
    check(bool(compute.post([&]{two.enter();})),"second target active"); two.wait();
    auto resource=std::make_shared<int>(1); std::weak_ptr<int> weak=resource;
    check(bool(compute.post([resource]{})),"second target has pending captures"); resource.reset();
    std::atomic<bool> closed=false;
    Thread closer;
    check(bool(closer.start([&](ThreadContext&){closed=bool(compute.close());})),"shutdown thread starts");
    const auto deadline=Clock::now()+2s;
    while(!weak.expired() && Clock::now()<deadline) std::this_thread::yield();
    const bool cancelled=weak.expired();
    one.release(); two.release(); check(bool(closer.join()),"shutdown thread joins");
    check(cancelled && closed,"all targets are cancelled before waiting for the first active target");
    check(compute.stats().completed==2 && compute.stats().cancelled==1,"two-phase shutdown preserves exact accounting");
}

void parallel_drain_and_query() {
    Runtime first({.workers=2,.capacity=16}), second({.workers=2,.capacity=16});
    LoadBalancer balancer({.capacity=16});
    const auto a=balancer.add_target(first,{.capacity=8}).value();
    const auto b=balancer.add_target(second,{.capacity=8}).value();
    Gate active;
    check(bool(balancer.post([&]{active.enter();})),"active work admitted"); active.wait();
    check(balancer.request_drain(a),"target stops admitting");
    std::atomic<int> total=0;
    QueryPort<int,int> query([](const int& n){return n*2;});
    check(bool(query.post(balancer,21,[&](auto reply){if(reply)total+=*reply;})),"common TaskExecutor query integration");
    std::array<Thread,4> producers;
    for(auto& producer:producers) check(bool(producer.start([&](ThreadContext&) {
        for(int i=0;i<100;++i) {
            auto posted=balancer.post([&]{++total;});
            if(!posted) check(posted.error==SubmitError::overloaded || posted.error==SubmitError::full,"bounded concurrent rejection");
        }
    })),"producer thread starts");
    for(auto& producer:producers) check(bool(producer.join()),"producer thread joins");
    const auto busy=first.post([&] {
        const auto result=balancer.wait_idle();
        check(!result && result.error().code==ThreadErrorCode::would_block,"worker cannot wait on another active target");
    });
    check(bool(busy),"worker wait probe");
    // first has a second worker, so this non-balancer probe is complete before releasing the gate.
    while(busy.token.pending()) std::this_thread::yield();
    const bool independent=bool(second.wait_idle()) && total>=42;
    active.release();
    check(independent,"second native pool completes while the first pool's work is blocked");
    check(bool(balancer.remove_target(a,ShutdownMode::drain)),"active target drains without cancelling work");
    check(bool(balancer.wait_idle()) && total>=42,"independent workers and query completion finish");
    check(balancer.stats().high_watermark<=16 && balancer.target_stats(b)->in_flight==0,"global admission stays bounded");
    check(bool(balancer.close(ShutdownMode::drain)),"global graceful drain");
}
}
static_assert(TaskExecutor<LoadBalancer>);
int main() {
    try {
        routing_and_limits(); health_and_resource_selection(); cancellation_and_retirement();
        rejection_inline_and_failure(); parallel_drain_and_query();
        operation_and_shutdown(); active_removal();
        request_close_races_registration(); cancel_all_before_waiting();
        std::cout<<"native computation load balancer contracts passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
