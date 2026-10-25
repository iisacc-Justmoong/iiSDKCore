#include <iiSDKCore/Runtime/LoadRouting.h>
#include <iiSDKCore/Runtime/EventLoop.h>
#include <array>
#include <atomic>
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace ii::runtime;
namespace {
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void reservations_and_lifetime() {
    EventLoop loop(16);
    LoadBalancer balance({.capacity=2});
    const auto target=balance.add_target(loop,{.capacity=2}).value();
    auto lease=balance.acquire().value();
    check(lease.target()==target && balance.stats().reservations==1,"async work must retain its reservation");
    bool ran=false;
    auto resource=std::make_shared<int>(42); std::weak_ptr<int> weak=resource;
    check(bool(lease.post([resource,&ran]{ran=true;})),"pinned executor submission");
    resource.reset(); lease.complete();
    check(balance.stats().in_flight==1 && !weak.expired(),"completion cannot retire queued callback captures");
    check(loop.poll()==1 && ran && weak.expired(),"pinned callback executes and destroys captures");
    check(balance.stats().completed==1 && balance.stats().reservations==0,"one logical request is settled once");
    auto held=balance.acquire().value();
    auto moved=std::move(held);
    check(!held && bool(moved),"reservation has move-only ownership");
    const auto closed=balance.close();
    check(!closed && closed.error().code==ThreadErrorCode::would_block && moved.stop_token().stop_requested(),
          "external async work receives cancellation without blocking its owner");
    moved.cancel();
    check(bool(balance.close()) && balance.stats().cancelled==1,"release completes the final shutdown barrier");
}
void group_and_policy_contracts() {
    EventLoop a(32),b(32),c(32),d(32);
    LoadBalancer balance;
    const auto one=balance.add_target(a,{.capacity=4,.group=1}).value();
    const auto two=balance.add_target(b,{.capacity=32,.group=1}).value();
    const auto three=balance.add_target(c,{.capacity=8,.group=2}).value();
    const auto four=balance.add_target(d,{.capacity=8,.group=2}).value();
    for(int i=0;i<8;++i) {
        auto left=balance.acquire({.group=1,.policy=BalancePolicy::round_robin}).value();
        auto right=balance.acquire({.group=2,.policy=BalancePolicy::round_robin}).value();
        check(left.target()==(i%2 ? two : one) && right.target()==(i%2 ? four : three),
              "target-group round robin must be independent");
        left.complete(); right.complete();
    }
    auto first=balance.acquire({.group=1,.policy=BalancePolicy::round_robin}).value();
    auto second=balance.acquire({.group=1,.policy=BalancePolicy::round_robin}).value();
    auto third=balance.acquire({.group=1,.policy=BalancePolicy::round_robin}).value();
    auto least=balance.acquire({.group=1,.policy=BalancePolicy::least_outstanding}).value();
    check(least.target()==two,"least outstanding counts requests instead of declared queue capacity");
    first.complete(); second.complete(); third.complete(); least.complete();
    check(balance.acquire({.group=99}).error()==SubmitError::overloaded,"groups cannot spill into unrelated services");
    check(balance.acquire({.policy=BalancePolicy::flow_hash}).error()==SubmitError::invalid,"hash routing requires an explicit key");
    check(balance.acquire({.policy=static_cast<BalancePolicy>(999)}).error()==SubmitError::invalid,"invalid policy rejected");
    auto pinned=balance.acquire({.group=1}).value();
    const auto chosen=pinned.target();
    check(balance.request_drain(chosen),"draining starts");
    bool delivered=false;
    check(bool(pinned.post([&]{delivered=true;})),"existing request can finish on its draining target");
    auto removing=balance.remove_target(chosen,ShutdownMode::drain);
    check(!removing && removing.error().code==ThreadErrorCode::would_block,"drain never waits on caller-owned reservation");
    a.poll(); b.poll();
    pinned.complete();
    check(delivered && bool(balance.remove_target(chosen,ShutdownMode::drain)),"drain removes target after request completion");
}
void reservation_failure_and_cleanup() {
    EventLoop loop(8);
    LoadBalancer balance;
    const auto target=balance.add_target(loop,{.capacity=8}).value();
    auto request=balance.acquire().value();
    check(bool(request.post([]{throw std::runtime_error("response failure");})),"failing async callback admitted");
    try { loop.poll(); } catch(const std::runtime_error&) {}
    request.complete();
    const auto failure=balance.wait_idle();
    check(!failure && failure.error().code==ThreadErrorCode::work_failed && balance.stats().failed==1,
          "a callback failure cannot be overwritten by completion");
    check(bool(balance.remove_target(target))==false,"target failure survives removal");
    LoadBalancer explicit_failure;
    (void)explicit_failure.add_target(loop,{.capacity=8}).value();
    auto failed=explicit_failure.acquire().value(); failed.fail();
    check(!explicit_failure.wait_idle() && explicit_failure.stats().failed==1,"transport failures without exception remain failures");
}
void application_rules() {
    EventLoop api(16),images(16),fallback(16);
    ApplicationLoadBalancer balance;
    const auto a=balance.targets().add_target(api,{.capacity=16,.group=1}).value();
    const auto b=balance.targets().add_target(images,{.capacity=16,.group=2}).value();
    const auto c=balance.targets().add_target(fallback,{.capacity=16,.group=3}).value();
    std::vector<ApplicationRoute> rules{
        {.priority=20,.group=2,.host="*.example.com",.path_prefix="/images/"},
        {.priority=10,.group=1,.host="api.example.com",.path_prefix="/v1/",.method="POST",
         .headers={{"x-mode","fast"}},.query={{"format","json"}}}
    };
    check(bool(balance.configure(rules,3)),"routing snapshot configured");
    const std::array headers{LoadField{"X-Mode","fast"}};
    const std::array query{LoadField{"format","json"}};
    auto one=balance.acquire({.host="API.EXAMPLE.COM",.path="/v1/jobs",.method="POST",.headers=headers,.query=query}).value();
    check(one.target()==a,"L7 host, path, method, header and query conditions select a group"); one.complete();
    auto two=balance.acquire({.host="cdn.example.com",.path="/images/a.png",.method="GET"}).value();
    check(two.target()==b,"host wildcard and path prefix"); two.complete();
    auto three=balance.acquire({.host="api.example.com",.path="/v1/jobs",.method="GET"}).value();
    check(three.target()==c,"default group applies when a condition fails"); three.complete();
    rules.push_back(rules.front());
    check(!balance.configure(std::move(rules),1),"duplicate rule priorities rejected transactionally");
    auto unchanged=balance.acquire({.host="cdn.example.com",.path="/images/a.png"}).value();
    check(unchanged.target()==b,"rejected configuration preserves the live snapshot"); unchanged.complete();
    check(balance.targets().request_drain(b),"matched group drains");
    check(balance.acquire({.host="cdn.example.com",.path="/images/a.png"}).error()==SubmitError::overloaded,
          "unavailable matched route cannot fall through to an unrelated service");
    check(balance.acquire({.path="bad-path"}).error()==SubmitError::invalid,"invalid metadata rejected");
    ApplicationLoadBalancer bounded({.max_rules=1,.max_conditions=1,.max_metadata_bytes=32});
    check(!bounded.configure({{.priority=1,.headers={{"x","1"},{"y","2"}}}}),"condition count bounded");
    check(bounded.acquire({.host=std::string(33,'x'),.path="/"}).error()==SubmitError::invalid,"request metadata bounded");
}
void classic_protocols() {
    EventLoop first(32),second(32);
    ClassicLoadBalancer balance;
    const auto a=balance.targets().add_target(first,{.capacity=16}).value();
    const auto b=balance.targets().add_target(second,{.capacity=16}).value();
    auto one=balance.acquire(ClassicProtocol::tcp).value();
    auto two=balance.acquire(ClassicProtocol::tls).value();
    auto three=balance.acquire(ClassicProtocol::tcp).value();
    check(one.target()==a && two.target()==b && three.target()==a,"Classic TCP/TLS listeners use round robin");
    auto http=balance.acquire(ClassicProtocol::http).value();
    check(http.target()==b,"Classic HTTP uses least outstanding requests");
    one.complete(); two.complete(); three.complete(); http.complete();
    check(balance.acquire(static_cast<ClassicProtocol>(999)).error()==SubmitError::invalid,"invalid Classic listener rejected");
}
void concurrent_requests_and_snapshots() {
    EventLoop first,second;
    ApplicationLoadBalancer balance;
    const auto a=balance.targets().add_target(first,{.capacity=64,.group=1}).value();
    const auto b=balance.targets().add_target(second,{.capacity=64,.group=2}).value();
    check(bool(balance.configure({{.priority=1,.group=1,.host="api.example.com"}},2)),"initial concurrent route");
    std::atomic<bool> failed=false;
    std::array<std::thread,4> producers;
    for(auto& producer:producers) producer=std::thread([&] {
        for(int i=0;i<500;++i) {
            auto request=balance.acquire({.host="api.example.com"});
            if(!request || (request->target()!=a && request->target()!=b)) failed=true;
            if(request) request->complete();
        }
    });
    for(int i=0;i<500;++i)
        if(!balance.configure({{.priority=1,.group=i%2 ? 1U : 2U,.host="api.example.com"}},2)) failed=true;
    for(auto& producer:producers) producer.join();
    check(!failed && balance.targets().stats().completed==2000,"concurrent readers see complete routing snapshots");

    Runtime runtime({.workers=4,.capacity=64});
    LoadBalancer requests({.capacity=1});
    (void)requests.add_target(runtime,{.capacity=1}).value();
    auto request=requests.acquire().value();
    std::atomic<int> callbacks=0;
    for(int i=0;i<32;++i) check(bool(request.post([&,i] {
        ++callbacks;
        if(i%7==0) throw std::runtime_error("parallel response failure");
    })),"parallel callbacks belong to one request");
    request.complete();
    const auto result=requests.wait_idle();
    check(!result && callbacks==32 && requests.stats().failed==1 && requests.stats().in_flight==0,
          "parallel callback failures settle one logical request exactly once");
}
struct Simulation final { std::size_t peak=0,pending=0,completed=0; };
Simulation simulate(BalancePolicy policy) {
    EventLoop fast,slow;
    LoadBalancer balance;
    const auto a=balance.add_target(fast,{.capacity=64}).value();
    const auto b=balance.add_target(slow,{.capacity=64}).value();
    std::deque<LoadLease> left,right;
    Simulation result;
    for(int tick=0;tick<512;++tick) {
        auto request=balance.acquire({.policy=policy}).value();
        (request.target()==a ? left : right).push_back(std::move(request));
        result.peak=std::max(result.peak,left.size()+right.size());
        if(!left.empty()) { left.front().complete(); left.pop_front(); ++result.completed; }
        if(tick%8==0 && !right.empty()) { right.front().complete(); right.pop_front(); ++result.completed; }
    }
    result.pending=left.size()+right.size();
    for(auto& lease:left) lease.complete();
    for(auto& lease:right) lease.complete();
    check(bool(balance.close(ShutdownMode::drain)) && balance.stats().completed==512,"simulation settles every request");
    (void)b;
    return result;
}
void uneven_service_optimization() {
    const auto baseline=simulate(BalancePolicy::round_robin);
    const auto optimized=simulate(BalancePolicy::least_outstanding);
    check(optimized.pending<baseline.pending && optimized.peak<baseline.peak && optimized.completed>baseline.completed,
          "least outstanding reduces backlog with unequal backend service rates");
    std::cout<<"uneven-service simulation: round_robin peak="<<baseline.peak<<" pending="<<baseline.pending
             <<" completed="<<baseline.completed<<"; least_outstanding peak="<<optimized.peak
             <<" pending="<<optimized.pending<<" completed="<<optimized.completed<<'\n';
}
}
static_assert(TaskExecutor<LoadLease>);
int main() {
    try {
        reservations_and_lifetime(); group_and_policy_contracts(); reservation_failure_and_cleanup();
        application_rules(); classic_protocols(); concurrent_requests_and_snapshots(); uneven_service_optimization();
        std::cout<<"Application and Classic load routing contracts passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
