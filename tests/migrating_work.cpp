#include <iiSDKCore/Runtime/MigratingWork.h>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace ii::runtime;
using namespace std::chrono_literals;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Gate final {
    std::mutex mutex;std::condition_variable cv;bool entered=false,open=false;
    void wait(){std::unique_lock lock(mutex);entered=true;cv.notify_all();if(!cv.wait_for(lock,5s,[&]{return open;}))throw std::runtime_error("gate timed out");}
    void entered_wait(){std::unique_lock lock(mutex);if(!cv.wait_for(lock,5s,[&]{return entered;}))throw std::runtime_error("step did not start");}
    void release(){std::lock_guard lock(mutex);open=true;cv.notify_all();}
};
std::vector<std::byte> byte(unsigned value){return {std::byte(value)};}
void migrate_running(){
    Runtime source({.workers=1,.capacity=4}),target({.workers=1,.capacity=4});Gate gate;
    std::thread::id before,after;std::atomic<bool> source_capture_retired=false,destination_entered=false;Gate cleanup;
    struct Marker final {std::atomic<bool>* retired;Gate* cleanup;~Marker(){cleanup->wait();*retired=true;}};
    auto marker=std::shared_ptr<Marker>(new Marker{&source_capture_retired,&cleanup});
    MigrationProgram first{"paint",1,[&,marker](std::span<const std::byte> bytes){before=std::this_thread::get_id();gate.wait();return MigrationStep{byte(unsigned(bytes[0])+1),false};}};marker.reset();
    MigrationProgram second{"paint",1,[&](std::span<const std::byte> bytes){
        destination_entered=true;check(source_capture_retired,"destination ran before source program captures retired");
        after=std::this_thread::get_id();return MigrationStep{byte(unsigned(bytes[0])+1),true};}};
    auto work=MigratingWork::start(source,std::move(first),byte(0)).value();gate.entered_wait();
    check(bool(work.migrate(target,std::move(second))),"migration request rejected");gate.release();
    cleanup.entered_wait();std::this_thread::sleep_for(20ms);const bool waited=!destination_entered;cleanup.release();
    auto status=work.wait(5s);check(status&&status->state==MigrationState::completed&&status->steps==2,"migrated task did not finish");
    check(waited&&before!=after&&work.result().value()==byte(2),"state, cleanup or executor was not transferred");
    check(bool(source.resize_workers(0)),"source workers could not be released after handoff");
}
void checkpoint_transfer(){
    Runtime source({.workers=1,.capacity=4}),target({.workers=1,.capacity=4});Gate gate;
    MigrationProgram initial{"paint",1,[&](std::span<const std::byte> value){gate.wait();return MigrationStep{byte(unsigned(value[0])+1),false};}};
    auto work=MigratingWork::start(source,std::move(initial),byte(4)).value();gate.entered_wait();
    check(bool(work.pause()),"pause request rejected");gate.release();
    auto paused=work.wait(5s);check(paused&&paused->state==MigrationState::paused&&paused->steps==1,"pause did not reach safe boundary");
    auto ticket=work.take_checkpoint().value();check(work.status().state==MigrationState::transferred,"source retained ownership after export");
    check(ticket.kind()=="paint"&&ticket.version()==1&&ticket.steps()==1,"checkpoint metadata missing");
    const auto encoded=ticket.encode();auto restored=WorkCheckpoint::decode(encoded,1024).value();
    auto truncated=encoded;truncated.pop_back();check(!WorkCheckpoint::decode(truncated,1024),"truncated checkpoint accepted");
    MigrationProgram wrong{"other",1,[](auto){return MigrationStep{byte(0),true};}};
    check(!MigratingWork::resume(target,std::move(wrong),restored)&&bool(restored),"wrong program consumed checkpoint");
    MigrationProgram correct{"paint",1,[](std::span<const std::byte> value){return MigrationStep{byte(unsigned(value[0])+1),true};}};
    auto resumed=MigratingWork::resume(target,std::move(correct),restored).value();
    check(!restored&&resumed.wait(5s)&&resumed.result().value()==byte(6),"restored checkpoint lost state or retained ownership");
}
void failed_target_preserves_checkpoint(){
    Runtime source({.workers=1,.capacity=4}),target({.workers=1,.capacity=4});check(bool(target.resize_workers(0)),"target pause");Gate gate;
    MigrationProgram initial{"paint",1,[&](std::span<const std::byte> value){gate.wait();return MigrationStep{byte(unsigned(value[0])+1),false};}};
    MigrationProgram next{"paint",1,[](std::span<const std::byte> value){return MigrationStep{byte(unsigned(value[0])+1),true};}};
    auto work=MigratingWork::start(source,std::move(initial),byte(0)).value();gate.entered_wait();
    check(bool(work.migrate(target,next)),"target request");gate.release();
    auto paused=work.wait(5s);check(paused&&paused->state==MigrationState::paused&&paused->steps==1,"failed handoff did not preserve a checkpoint");
    check(bool(target.resize_workers(1))&&bool(work.resume(target,std::move(next))),"resume after rejected target");
    check(work.wait(5s)&&work.result().value()==byte(2),"retry did not finish from preserved state");
}
void cancelled_queue_preserves_checkpoint(){
    Runtime source({.workers=1,.capacity=4}),target({.workers=1,.capacity=4});Gate gate;
    check(bool(source.post([&]{gate.wait();})),"occupy executor before queuing work");gate.entered_wait();
    MigrationProgram first{"paint",1,[](std::span<const std::byte> value){return MigrationStep{byte(unsigned(value[0])+1),true};}};
    auto work=MigratingWork::start(source,first,byte(7)).value();
    source.request_stop(ShutdownMode::cancel_pending);
    const auto paused=work.wait(5s);check(paused&&paused->state==MigrationState::paused&&paused->steps==0,"cancelled queued work lost its checkpoint");
    gate.release();check(bool(source.join()),"cancelled source drains");
    check(bool(work.resume(target,std::move(first)))&&work.wait(5s)&&work.result().value()==byte(8),"cancelled queue could not resume on another runtime");
}
}
int main(){try{migrate_running();checkpoint_transfer();failed_target_preserves_checkpoint();cancelled_queue_preserves_checkpoint();std::cout<<"MIGRATING_WORK_OK\n";}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
