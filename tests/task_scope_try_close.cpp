#include <iiSDKCore/Runtime/TaskScope.h>
#include <iiSDKCore/Runtime/Runtime.h>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
using namespace ii::runtime;
using namespace std::chrono_literals;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Gate final {
    std::mutex mutex;std::condition_variable changed;bool entered=false,released=false;
    void enter(){std::unique_lock lock(mutex);entered=true;changed.notify_all();if(!changed.wait_for(lock,5s,[&]{return released;}))throw std::runtime_error("Gate timed out");}
    void wait(){std::unique_lock lock(mutex);if(!changed.wait_for(lock,5s,[&]{return entered;}))throw std::runtime_error("Work did not start");}
    void release(){std::lock_guard lock(mutex);released=true;changed.notify_all();}
};
void queued(){
    EventLoop loop(4);int cleaned=0;TaskScope owner([&]{++cleaned;});
    auto value=std::make_shared<int>(3);std::weak_ptr<int> retired=value;
    check(bool(owner.at(loop,Clock::now()+1h,[value]{})),"Timed work was rejected");value.reset();
    check(bool(owner.try_close())&&retired.expired()&&loop.stats().pending==0&&cleaned==1,"Nonblocking close failed to cancel pending captures before cleanup");
    check(bool(owner.try_close())&&cleaned==1,"Cleanup ran twice");
}
void active(){
    Runtime runtime({.workers=1,.capacity=4});TaskScope owner;Gate gate;
    check(bool(owner.post(runtime,[&]{gate.enter();})),"Work was rejected");gate.wait();
    const auto result=owner.try_close();check(!result&&result.error().code==ThreadErrorCode::would_block&&owner.closed(),"An active nonblocking close waited or reported completion");
    check(owner.post(runtime,[]{}).error==SubmitError::stopped,"Nonblocking close left admission open");
    gate.release();check(bool(runtime.wait_idle())&&bool(owner.try_close()),"Quiescent nonblocking close did not complete");
}
void self(){
    TaskScope owner;check(owner.run([&]{const auto result=owner.try_close();check(!result&&result.error().code==ThreadErrorCode::self_join,"An active callback could close itself");})==SubmitError::none,"Synchronous work failed");
    check(bool(owner.try_close()),"Nonblocking close did not recover outside its callback");
}
void failure(){
    TaskScope owner([]{throw std::runtime_error("Cleanup failure");});
    for(int n=0;n<2;++n){const auto result=owner.try_close();check(!result&&result.error().code==ThreadErrorCode::work_failed&&bool(result.error().cause),"Cleanup failure was lost");}
}
struct Capture final {Gate& gate;~Capture(){gate.enter();}};
void captures(){
    Runtime runtime({.workers=1,.capacity=4});TaskScope owner;Gate gate,work;
    auto resource=std::make_shared<Capture>(gate);
    check(bool(owner.post(runtime,[resource,&work]{work.enter();})),"Capture work was rejected");work.wait();resource.reset();work.release();gate.wait();
    const auto result=owner.try_close();check(!result&&result.error().code==ThreadErrorCode::would_block,"A capture still being destroyed was reported as retired");
    gate.release();check(bool(runtime.wait_idle())&&bool(owner.try_close())&&runtime.stats().pending==0,"Final capture or executor quota did not retire");
}
}
int main(int argc,char** argv){try{const std::string name=argc>1?argv[1]:"queued";
    if(name=="queued")queued();else if(name=="active")active();else if(name=="self")self();else if(name=="failure")failure();else if(name=="captures")captures();else throw std::runtime_error("Unknown try_close test");
    std::cout<<"TaskScope try_close "<<name<<" passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
