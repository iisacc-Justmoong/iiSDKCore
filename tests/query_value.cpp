#include <iiSDKCore/Runtime/QueryValue.h>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#include <thread>
#endif
#include <vector>

using namespace ii;
using namespace std::chrono_literals;
namespace {
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct Read final {};
struct Write final {std::string value;};
struct Model final {
    std::string stored="Draft";
    int reads=0,writes=0;
    core::Observable<std::uint64_t> changed{0};
    runtime::QueryPort<Read,std::string> read{[this](const Read&){++reads;return stored;}};
    runtime::QueryPort<Write,core::Result<void>> write{[this](const Write& request)->core::Result<void>{
        ++writes;
        if(request.value.size()>5)return core::fail(core::ErrorCode::invalid_argument,"Name is too long");
        if(stored!=request.value){stored=request.value;changed.set(changed.get()+1);}
        return {};
    }};
    runtime::QueryValue<std::string,Read,Write> value{
        read,Read{},write,[](std::string next){return Write{std::move(next)};},changed};
};
void run(){
    {
        Model traced;std::vector<core::ObservableRead> dependencies;
        const auto value=core::ObservableReadScope::collect(
            [&](core::ObservableRead read){dependencies.push_back(std::move(read));},
            [&]{return traced.value.get();});
        check(value=="Draft"&&dependencies.size()==1,
              "A query read was omitted from the active dependency scope");
        int invalidations=0;auto tracked=dependencies.front().subscribe([&]{++invalidations;});
        check(bool(traced.value.set("Next"))&&invalidations==1,
              "A tracked query source did not invalidate its consumer");
    }
    Model model;
    check(model.value.get()=="Draft"&&model.reads==1,"Value did not use the read query");
    std::vector<std::string> received;
    auto lease=model.value.observe([&](const std::string& value,std::uint64_t){received.push_back(value);},true);
    check(received==std::vector<std::string>{"Draft"}&&model.reads==2,"Initial observation bypassed the read query");
    check(bool(model.value.set("Next"))&&model.stored=="Next"&&model.writes==1,
          "A value edit did not reach the write query");
    check(received==std::vector<std::string>({"Draft","Next"})&&model.reads==3,
          "A committed change did not requery the selected value");
    check(bool(model.value.set("Next"))&&received.size()==2&&model.reads==3,
          "An unchanged value triggered a new observation or read");
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    bool read_wrong_thread=false,write_wrong_thread=false;
    std::thread worker([&]{
        try{(void)model.value.get();}catch(const runtime::QueryValueFailure& error){
            read_wrong_thread=error.error().code==runtime::QueryErrorCode::wrong_executor;
        }
        const auto result=model.value.set("Bad");
        write_wrong_thread=!result&&result.error().code==core::ErrorCode::wrong_executor;
    });
    worker.join();
    check(read_wrong_thread&&write_wrong_thread&&model.reads==3&&model.writes==2,
          "Cross-thread access bypassed the owner's execution domain");
#endif
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    {
        runtime::Runtime runtime({.workers=1});runtime::SerialQueue queue(runtime);
        runtime::QueryPort<Read,std::string> foreign_read(queue,[](const Read&){return std::string("foreign");});
        runtime::QueryPort<Write,core::Result<void>> foreign_write(queue,[](const Write&)->core::Result<void>{return {};});
        runtime::QueryValue<std::string,Read,Write> foreign{
            foreign_read,Read{},foreign_write,[](std::string value){return Write{std::move(value)};},model.changed};
        try{(void)foreign.get();throw std::runtime_error("Foreign read was accepted");}
        catch(const runtime::QueryValueFailure& failure){
            check(failure.error().code==runtime::QueryErrorCode::wrong_executor,"Read port executor error was lost");
        }
        auto denied=foreign.set("Bad");
        check(!denied&&denied.error().code==core::ErrorCode::wrong_executor&&bool(denied.error().cause),
              "Write port executor error was lost");
    }
#endif
    auto rejected=model.value.set("excessive");
    check(!rejected&&rejected.error().code==core::ErrorCode::invalid_argument&&model.stored=="Next"&&received.size()==2,
          "A domain rejection was lost or changed the source");
    runtime::EventLoop loop(8);bool delivered=false;
    auto queued=model.write.post(loop,Write{"Peer"},[&](auto reply){delivered=bool(reply)&&bool(*reply);});
    check(bool(queued)&&model.stored=="Next","A queued write ran synchronously");
    loop.poll({8,1s});
    check(delivered&&model.stored=="Peer"&&received.back()=="Peer","Queued query did not update the value on its owner");
    lease=core::ObservableSubscription{};
    check(bool(model.value.set("End"))&&received.back()=="Peer","A retired observation was called");
    check(bool(model.read.close()),"Read port did not close");
    try{(void)model.value.get();throw std::runtime_error("Closed read was accepted");}
    catch(const runtime::QueryValueFailure& failure){
        check(failure.error().code==runtime::QueryErrorCode::receiver_expired,"Read transport failure lost its cause");
    }
    check(bool(model.write.close()),"Write port did not close");
    auto closed=model.value.set("Last");
    check(!closed&&closed.error().code==core::ErrorCode::closed&&bool(closed.error().cause),
          "Closed write did not preserve its transport error");
}
}
int main(){try{run();std::cout<<"QUERY_VALUE_OK\n";}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
