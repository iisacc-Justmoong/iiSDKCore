#include <iiSDKCore/Runtime/ComputeRouter.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace ii::runtime;
using namespace std::chrono_literals;
namespace {
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class Predicate> void until(Predicate done) {
    const auto deadline=Clock::now()+5s;
    while(!done() && Clock::now()<deadline) std::this_thread::sleep_for(1ms);
    check(done(),"router deadline elapsed");
}
struct Gate final {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered=false,released=false;
    void run() { std::unique_lock lock(mutex); entered=true; changed.notify_all(); check(changed.wait_for(lock,5s,[&]{return released;}),"gate release timed out"); }
    void wait() { std::unique_lock lock(mutex); check(changed.wait_for(lock,5s,[&]{return entered;}),"gate did not start"); }
    void release() { std::lock_guard lock(mutex); released=true; changed.notify_all(); }
};
const ComputeTargetSnapshot& target(const ComputeSnapshot& snapshot,LoadTarget id) {
    const auto found=std::ranges::find(snapshot.targets,id,&ComputeTargetSnapshot::target);
    if(found==snapshot.targets.end()) throw std::runtime_error("target snapshot absent");
    return *found;
}
void topology_and_live_assignment() {
    Runtime first({.workers=1,.capacity=16}),second({.workers=1,.capacity=16});
    ComputeRouter router({.max_zones=2,.max_scopes=3,.max_targets=2,.capacity=8});
    const auto east=router.add_zone({.name="east"}).value();
    const auto west=router.add_zone({.name="west"}).value();
    check(router.add_zone({.name="overflow"}).error()==SubmitError::full,"zone table bounded");
    const auto scope=router.add_scope({.name="service",.zones={east}}).value();
    const auto a=router.add_target(first,east,{.load={.capacity=8}}).value();
    const auto b=router.add_target(second,west,{.load={.capacity=8}}).value();
    auto old=router.acquire(scope).value();
    check(old.placement().target==a && old.placement().zone==east,"scope restricts eligible zones");
    check(bool(router.configure_scope(scope,{.name="service",.zones={west}})),"scope coverage changes live");
    auto next=router.acquire(scope).value();
    check(next.placement().target==b && old.placement().zone==east,"accepted work keeps its original allocation");
    check(bool(router.assign_target(b,east)),"target can move between zones while old work exists");
    check(next.placement().zone==west && router.acquire(scope).error()==SubmitError::overloaded,"move only changes new placement");
    check(router.remove_zone(west).error()==SubmitError::overloaded,"referenced or reserved zones cannot disappear");
    check(bool(router.configure_scope(scope,{.name="service"})),"scope can cover all dynamically registered zones");
    old.complete(); next.complete();
    check(bool(router.remove_zone(west)),"unused zone can be removed after old reservations retire");
    check(!router.configure_zone(west,{.name="stale"}),"removed identities never become valid again");
    check(bool(router.configure_zone(east,{.name="east",.enabled=false})),"zone admission can be disabled");
    check(router.acquire(scope).error()==SubmitError::overloaded,"disabled zones reject new work");
    check(bool(router.configure_zone(east,{.name="east"})),"zone can be re-enabled");
    const auto replacement=router.add_zone({.name="replacement"}).value();
    check(replacement!=west && bool(router.assign_target(b,replacement)),"zone slots reuse fresh identities");
    check(bool(router.configure_zone(east,{.name="east",.enabled=false})),"preferred zone unavailable");
    check(router.acquire(scope,{.preferred_zone=east,.cross_zone=false}).error()==SubmitError::overloaded,"cross-zone restriction enforced");
    auto fallback=router.acquire(scope,{.preferred_zone=east}).value();
    check(fallback.placement().zone==replacement,"cross-zone failover stays inside scope"); fallback.complete();
    ComputeRouter foreign;
    check(!foreign.configure_zone(east,{.name="foreign"}) && !router.remove_scope(router.default_scope()),"foreign/default identities protected");
    check(bool(router.close(ShutdownMode::drain)),"topology drains");
}
void bounds_and_filtered_selection() {
    EventLoop loop;
    LoadBalancer balance;
    const auto id=balance.add_target(loop,{.capacity=4}).value();
    const std::array allowed{id};
    check(balance.acquire({},std::span<const LoadTarget>{}).error()==SubmitError::overloaded,"empty explicit selection cannot fall through");
    auto selected=balance.acquire({},allowed).value(); check(selected.target()==id,"restricted candidate accepted"); selected.complete();
    Runtime runtime({.workers=1,.capacity=8});
    ComputeRouter router;
    const auto zone=router.add_zone({.name="limited",.limits={.jobs=1,.units=4,.memory_bytes=32}}).value();
    (void)router.add_target(runtime,zone,{.load={.capacity=8,.memory_capacity=64}}).value();
    auto held=router.acquire(router.default_scope(),{.work={.units=2,.memory_bytes=16}}).value();
    check(router.acquire(router.default_scope()).error()==SubmitError::overloaded,"zone budget spans targets");
    check(bool(router.configure_zone(zone,{.name="limited",.limits={.jobs=1,.units=1,.memory_bytes=8}})),"live quota may shrink below existing use");
    check(router.monitor().zones.front().overcommitted,"overcommit is observed without cancelling accepted work");
    held.complete();
    check(!router.monitor().zones.front().overcommitted,"quota is returned to the original zone");
    auto request=router.acquire(router.default_scope()).value();
    const auto closing=router.close();
    check(!closing && closing.error().code==ThreadErrorCode::would_block && request.stop_token().stop_requested(),"external allocations get a nonblocking close request");
    request.cancel(); check(bool(router.close()),"close retries after external cleanup");
}
void monitoring_and_processing() {
    Runtime busy({.workers=1,.capacity=16}),free({.workers=1,.capacity=16});
    auto gate=std::make_shared<Gate>();
    check(bool(busy.post([gate]{gate->run();})),"foreign runtime work starts"); gate->wait();
    ComputeRouter router;
    const auto zone=router.add_zone({.name="workers"}).value();
    const auto a=router.add_target(busy,zone,{.load={.capacity=16}}).value();
    const auto b=router.add_target(free,zone,{.load={.capacity=16}}).value();
    auto chosen=router.acquire(router.default_scope()).value();
    const bool idle_chosen=chosen.placement().target==b;
    const auto snapshot=router.monitor();
    const bool observed=target(snapshot,a).runtime.active==1 && target(snapshot,a).runtime.pending==1;
    chosen.complete(); gate->release(); check(bool(busy.wait_idle()),"foreign job finishes");
    check(idle_chosen && observed,"actual worker pressure changes routing and is visible");
    std::atomic<int> calls=0;
    for(int i=0;i<20;++i) check(bool(router.post([&]{++calls;})),"router is a TaskExecutor");
    check(bool(router.close(ShutdownMode::drain)) && calls==20,"drain waits for owned internal work");
    const auto done=router.monitor();
    check(done.load.completed==21 && std::ranges::all_of(done.targets,[](const auto& item){return item.active_callbacks==0;}),"processing counters settle after captures retire");
}
void observations_and_timer() {
    Runtime runtime({.workers=1,.capacity=16});
    ComputeRouter router;
    const auto zone=router.add_zone({.name="observed"}).value();
    const auto id=router.add_target(runtime,zone,{.load={.capacity=16},.require_observation=true,.cpu_ceiling=0.9}).value();
    check(router.acquire(router.default_scope()).error()==SubmitError::overloaded,"required observations fail closed");
    check(router.report_load(id,{.cpu_utilization=0.1,.sampled_at=Clock::now()-2s,.valid_for=1s}),"old sample is retained with its timestamp");
    check(!target(router.monitor(),id).observation_fresh && router.acquire(router.default_scope()).error()==SubmitError::overloaded,"stale samples are visible and excluded");
    check(!router.report_load(id,{.cpu_utilization=std::numeric_limits<double>::quiet_NaN()}),"invalid metrics rejected");
    check(router.report_load(id,{.cpu_utilization=0.95}),"fresh pressure sample accepted");
    check(router.acquire(router.default_scope()).error()==SubmitError::overloaded,"CPU ceiling enforced");
    check(router.report_load(id,{.cpu_utilization=0.1,.external_memory_bytes=20,.memory_capacity=32}),"external memory observation");
    check(router.acquire(router.default_scope(),{.work={.memory_bytes=16}}).error()==SubmitError::overloaded,"memory pressure constrains admission");
    auto fits=router.acquire(router.default_scope(),{.work={.memory_bytes=8}}).value(); fits.complete();
    EventLoop loop(8);
    const auto before=router.monitor().monitor_samples;
    check(bool(router.start_monitoring(loop,1ms)),"periodic monitoring uses the existing loop");
    until([&]{loop.poll(); return loop.stats().executed>=2;});
    check(bool(router.stop_monitoring()) && loop.stats().pending==0 && router.monitor().monitor_samples>=before+3,"monitor timer and captures are retired");
    check(bool(router.close(ShutdownMode::drain)),"monitor owner closes");
}
void concurrent_reconfiguration() {
    Runtime first({.workers=1,.capacity=16}),second({.workers=1,.capacity=16});
    ComputeRouter router;
    const auto a=router.add_zone({.name="a"}).value();
    const auto b=router.add_zone({.name="b"}).value();
    const auto target_a=router.add_target(first,a,{.load={.capacity=16}}).value();
    (void)router.add_target(second,b,{.load={.capacity=16}}).value();
    std::atomic<bool> failed=false;
    std::array<std::thread,3> producers;
    for(auto& thread:producers) thread=std::thread([&] {
        for(int i=0;i<300;++i) {
            auto request=router.acquire(router.default_scope());
            if(!request) { failed=true; continue; }
            const auto placement=request->placement();
            if(placement.zone!=a && placement.zone!=b) failed=true;
            request->complete();
        }
    });
    for(int i=0;i<300;++i) {
        if(!router.assign_target(target_a,i%2 ? a : b)) failed=true;
        (void)router.monitor();
    }
    for(auto& thread:producers) thread.join();
    check(!failed && bool(router.close(ShutdownMode::drain)) && router.monitor().load.completed==900,"concurrent allocation, observation and reassignment are consistent");
}
void native_processes(const std::string& fixture) {
    if(!ii::process::capabilities().launch) { std::cout<<"native process launch unsupported on this target\n"; return; }
    Runtime runtime({.workers=2,.capacity=16});
    ComputeRouter router;
    const auto zone=router.add_zone({.name="native-processes"}).value();
    const auto id=router.add_target(runtime,zone,{.load={.capacity=16}}).value();
    ii::process::Launch launch; launch.arguments={fixture,"echo","routed"}; launch.input=ii::process::Input::closed;
    auto child=router.route_process(router.default_scope(),launch).value();
    check(child.operation().wait_for(5s)==OperationWait::ready,"routed process completes");
    auto result=child.operation().result();
    check(result && result->value && result->value->exit.code==0 && child.native_id()!=0 && child.placement().target==id,"process result retains native identity and placement");
    const auto& bytes=result->value->output;
    check(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size())=="routed","real child output crosses the routed operation");
    launch.arguments={fixture,"sleep"};
    auto cancelled=router.route_process(router.default_scope(),launch).value();
    until([&]{return cancelled.native_id()!=0;});
    check(cancelled.operation().cancel() && cancelled.operation().wait_for(5s)==OperationWait::ready,"operation cancellation reaches a live child");
    const auto cancelled_result=cancelled.operation().result();
    check(cancelled_result && cancelled_result->value && cancelled_result->value->exit.reason==ii::process::ExitReason::cancelled,
          "cancellation retains the actual process exit result");
    ii::process::CommunicationOptions timed_options;
    timed_options.deadline=ii::core::Deadline::after(50ms);
    auto timed=router.route_process(router.default_scope(),launch,{},timed_options).value();
    check(timed.operation().wait_for(5s)==OperationWait::ready,"routed process deadline settles");
    const auto timed_result=timed.operation().result();
    check(timed_result && ((timed_result->value && timed_result->value->exit.reason==ii::process::ExitReason::timed_out) ||
          timed_result->status==OperationStatus::timed_out),"deadline applies before or after native launch");
    auto active=router.route_process(router.default_scope(),launch).value();
    until([&]{return active.native_id()!=0;});
    check(target(router.monitor(),id).active_processes==1,"active native process is monitored");
    check(bool(router.close()),"router shutdown cancels and reaps owned native processes");
    auto stopped=active.operation().result();
    check(stopped && stopped->value && stopped->value->exit.reason==ii::process::ExitReason::owner_removed,"owner removal survives process cleanup");
    check(router.monitor().load.in_flight==0,"native process resources return after reaping");

    ComputeRouter failures;
    const auto failure_zone=failures.add_zone({.name="spawn-errors"}).value();
    (void)failures.add_target(runtime,failure_zone).value();
    launch.arguments={fixture+".does-not-exist"};
    auto failed=failures.route_process(failures.default_scope(),launch).value();
    check(failed.operation().wait_for(5s)==OperationWait::ready && failed.operation().result()->status==OperationStatus::failed && !failed.native_id(),
          "spawn failure remains an observable operation error");
    const auto closed=failures.close();
    check(!closed && closed.error().code==ThreadErrorCode::work_failed && failures.monitor().load.in_flight==0,
          "failed process allocation still completes resource cleanup");
}
}
static_assert(TaskExecutor<ComputeRouter>);
static_assert(TaskExecutor<ComputeLease>);
int main(int argc,char** argv) {
    try {
        topology_and_live_assignment(); bounds_and_filtered_selection(); monitoring_and_processing();
        observations_and_timer(); concurrent_reconfiguration();
        if(argc>1) native_processes(argv[1]);
        std::cout<<"dynamic availability, monitoring and process routing contracts passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
