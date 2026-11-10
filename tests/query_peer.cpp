#include <iiSDKCore/Runtime/QueryPeer.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#include <latch>
#include <thread>
#endif

using namespace ii::runtime;
using namespace std::chrono_literals;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct LeftRequest final {int value;};
struct RightRequest final {int value;};
using Left=QueryPeer<RightRequest,int,LeftRequest,std::string>;
using Right=QueryPeer<LeftRequest,std::string,RightRequest,int>;

void symmetric_exchange(){
    int received=0;
    Left left([&](const RightRequest& q){received=q.value;return q.value+10;});
    Right right([](const LeftRequest& q){return std::to_string(q.value*2);});
    check(!left.send({1})&&left.post({1},[](auto){}).error==SubmitError::stopped,"Disconnected node admitted a query");
    check(bool(left.connect(right))&&left.connected()&&right.connected(),"Peer connection failed");
    check(left.send({21}).value()=="42"&&right.send({7}).value()==17&&received==7,"The two independently initiated queries did not reach their peer");
    Right other([](const LeftRequest&){return std::string("other");});
    auto busy=left.connect(other);check(!busy&&busy.error()==QueryPeerConnectError::busy,"Connected peer accepted another link");
    left.disconnect();check(!left.connected()&&!right.connected(),"Disconnect did not retire both directions");
    check(bool(left.connect(other))&&left.send({1}).value()=="other"&&!right.send({1}),"Reconnection reused the old peer");
    QueryPeer<int,int> same([](int n){return n;});auto self=same.connect(same);
    check(!self&&self.error()==QueryPeerConnectError::same_node,"A node linked to itself");
    int noticed=0;
    QueryPeer<int,void,double,std::unique_ptr<int>> notifier([&](int value){noticed=value;});
    QueryPeer<double,std::unique_ptr<int>,int,void> owner([](double value){return std::make_unique<int>(int(value));});
    check(bool(notifier.connect(owner)),"Different void and owning response types did not connect");
    auto owned=notifier.send(6.0);
    check(owned&&**owned==6&&bool(owner.send(8))&&noticed==8,"Void or move-only peer response was lost");
}

void asynchronous_turns(){
    EventLoop left_loop(8),right_loop(8);int left_seen=0,right_seen=0,completion=0;
    Left left([&](const RightRequest& q){left_seen=q.value;return q.value+1;});
    Right* target=nullptr;
    Right right([&](const LeftRequest& q){
        right_seen=q.value;
        if(target->connected())check(bool(target->post(left_loop,{q.value+2},[&](QueryReply<int> r){completion=r.value();})),"Peer could not initiate a reverse query");
        return std::to_string(q.value);
    });target=&right;
    check(bool(left.connect(right)),"Could not link async peers");
    std::string answer;check(bool(left.post(right_loop,{40},[&](QueryReply<std::string> r){answer=r.value();})),"Could not post to peer");
    check(!left_seen&&!right_seen,"Asynchronous delivery ran inline");
    right_loop.poll({8,1s});check(right_seen==40&&answer=="40"&&!left_seen,"First peer did not receive its request");
    left_loop.poll({8,1s});check(left_seen==42&&completion==43,"Independent reverse query or its reply was lost");
    bool old_completion=false;check(bool(left.post(right_loop,{5},[&](auto){old_completion=true;})),"Could not queue old link");
    left.disconnect();Right replacement([](const LeftRequest&){return std::string("new");});
    check(bool(left.connect(replacement)),"Could not replace link");
    right_loop.poll({8,1s});left_loop.poll({8,1s});
    check(old_completion&&right_seen==5&&left.send({0}).value()=="new","Admitted work moved to a replacement peer");
}

void nested_exchange(){
    int called=0;Left left([&](const RightRequest& q){++called;return q.value+2;});
    Right* self=nullptr;
    Right right([&](const LeftRequest& q){
        auto nested=self->send({q.value});
        check(nested&&*nested==q.value+2,"Reentrant reverse query failed");
        self->disconnect();
        return std::to_string(*nested);
    });self=&right;
    check(bool(left.connect(right)),"Could not link reentrant peers");
    auto result=left.send({5});
    check(result&&*result=="7"&&called==1&&!left.connected()&&!right.connected(),"A callback could not send and disconnect reentrantly");
}

void closed_and_canceled(){
    EventLoop loop(8);Left left([](const RightRequest& n){return n.value;});int handled=0;bool completed=false;
    auto right=std::make_unique<Right>([&](const LeftRequest&){++handled;return std::string("ok");});
    check(bool(left.connect(*right)),"Could not link lifetime fixture");
    check(bool(left.post(loop,{1},[&](auto){completed=true;})),"Could not queue lifetime fixture");
    right.reset();loop.poll({8,1s});
    check(!handled&&!completed&&!left.connected()&&!left.send({2}),"Retired peer delivered a queued request or retained a live link");
    Right closed([](const LeftRequest&){return std::string{};});check(bool(closed.close()),"Idle peer did not close");
    auto link=left.connect(closed);check(!link&&link.error()==QueryPeerConnectError::closed,"Closed peer accepted a new connection");
}

void failure_and_backpressure(){
    Left left([](const RightRequest& n){return n.value;});
    Right right([](const LeftRequest&)->std::string{throw std::runtime_error("peer handler");});
    check(bool(left.connect(right)),"Could not link failing peers");
    auto failure=left.send({1});
    check(!failure&&failure.error().code==QueryErrorCode::handler_failed&&failure.error().cause,"Peer failure lost its typed cause");
    EventLoop full(1);check(bool(full.post([]{})),"Could not occupy bounded queue");
    bool completed=false;
    check(left.post(full,{1},[&](auto){completed=true;}).error==SubmitError::full&&!completed,"Peer hid queue backpressure");
    full.poll({8,1s});
    check(bool(left.post(full,{1},[&](auto result){completed=!result&&result.error().code==QueryErrorCode::handler_failed;})),"Peer did not enqueue handler failure");
    full.poll({8,1s});check(completed,"Peer lost asynchronous handler failure");
}

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
void racing_connection(){
    for(int attempt=0;attempt<32;++attempt){
        QueryPeer<int,int> a([](int n){return n;}),b([](int n){return n+1;}),c([](int n){return n+2;});
        std::latch ready(2),start(1);std::atomic<int> accepted=0,busy=0;
        const auto race=[&](auto& other){ready.count_down();start.wait();auto result=a.connect(other);
            if(result)++accepted;else if(result.error()==QueryPeerConnectError::busy)++busy;};
        std::thread first([&]{race(b);}),second([&]{race(c);});ready.wait();start.count_down();first.join();second.join();
        check(accepted==1&&busy==1&&a.connected()&&(b.connected()!=c.connected()),"Racing connections produced a partial or shared link");
        a.disconnect();check(!b.connected()&&!c.connected(),"Racing connection left a live stale peer");
    }
}

void serial_domains(){
    Runtime runtime({.workers=2,.capacity=16});SerialQueue left_queue(runtime),right_queue(runtime);
    std::atomic<int> replies=0;
    Left left(left_queue,[&](const RightRequest& q){check(left_queue.is_current(),"Left receiver left its queue");return q.value;});
    Right right(right_queue,[&](const LeftRequest& q){check(right_queue.is_current(),"Right receiver left its queue");return std::to_string(q.value);});
    check(bool(left.connect(right)),"Could not link serialized peers");
    auto invalid=left.send({1});check(!invalid&&invalid.error().code==QueryErrorCode::wrong_executor,"Direct cross-queue call bypassed serialization");
    check(bool(left.post({4},[&](auto r){if(r&&*r=="4")++replies;})),"Left async query was rejected");
    check(bool(right.post({5},[&](auto r){if(r&&*r==5)++replies;})),"Right async query was rejected");
    check(bool(runtime.wait_idle())&&replies==2,"Bidirectional serial requests did not complete");
}

void retirement_and_reentry(){
    Left* local=nullptr;bool rejected=false;
    Left left([&](const RightRequest& q){auto result=local->close();rejected=!result&&result.error().code==ThreadErrorCode::self_join;return q.value;});local=&left;
    Right right([](const LeftRequest&){return std::string{};});
    check(bool(left.connect(right))&&right.send({8}).value()==8&&rejected&&left.connected(),"Self-close damaged an active peer connection");
    left.disconnect();
    std::latch entered(1),release(1);std::atomic<bool> closed=false;
    auto receiver=std::make_unique<Right>([&](const LeftRequest&){entered.count_down();release.wait();return std::string("finished");});
    check(bool(left.connect(*receiver)),"Could not link active retirement fixture");
    std::thread sender([&]{check(left.send({1}).value()=="finished","Active peer reply disappeared");});entered.wait();
    std::thread closer([&]{receiver.reset();closed=true;});
    release.count_down();sender.join();closer.join();
    check(closed&&!left.connected()&&!left.send({1}),"Concurrent receiver retirement left a usable peer");
}

void nonblocking_retirement(){
    Left left([](const RightRequest& q){return q.value;});
    std::latch entered(1),release(1);std::atomic<bool> replied=false;
    Right right([&](const LeftRequest&){entered.count_down();release.wait();return std::string("finished");});
    check(bool(left.connect(right)),"Could not link nonblocking retirement fixture");
    std::thread sender([&]{auto result=left.send({1});replied=result&&*result=="finished";});entered.wait();
    auto pending=right.try_close();
    const bool blocked=!pending&&pending.error().code==ThreadErrorCode::would_block;
    const bool disconnected=!left.connected()&&!right.connected()&&!left.send({2});
    release.count_down();sender.join();
    check(blocked&&disconnected&&replied&&bool(right.close()),"Nonblocking close did not retire admission before its active handler");
}
#endif
}
int main(){
    symmetric_exchange();asynchronous_turns();nested_exchange();closed_and_canceled();failure_and_backpressure();
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    racing_connection();serial_domains();retirement_and_reentry();nonblocking_retirement();
#endif
}
