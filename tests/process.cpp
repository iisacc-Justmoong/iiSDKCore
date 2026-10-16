#include <iiSDKCore/Process/Process.h>
#include <iiSDKCore/Runtime/EventLoop.h>
#include <array>
#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__) && !TARGET_OS_IPHONE
#include <libproc.h>
#include <sys/proc.h>
#endif
#endif
namespace {
using namespace ii::process;
using namespace std::chrono_literals;
int checks=0;
void check(bool value,std::string_view message) {
    ++checks; if(!value) throw std::runtime_error(std::string(message)+" ("+std::to_string(checks)+")");
}
template<class T> T take(ii::core::Result<T>&& value) {
    if(!value) throw std::runtime_error(value.error().message+" after check "+std::to_string(checks)); return std::move(*value);
}
std::string text(const ii::core::Bytes& bytes) { return {reinterpret_cast<const char*>(bytes.data()),bytes.size()}; }
std::string utf8(const std::filesystem::path& path) { const auto s=path.u8string(); return {reinterpret_cast<const char*>(s.data()),s.size()}; }
Launch launch(const std::string& fixture,std::initializer_list<std::string> args) {
    Launch value; value.arguments={fixture}; value.arguments.insert(value.arguments.end(),args); return value;
}
Communication run(Launch value,ii::core::ByteView input={},CommunicationOptions options={}) {
    if(options.deadline.remaining()==ii::core::Clock::duration::max()) options.deadline=ii::core::Deadline::after(10s);
    auto child=take(Process::start(value)); return take(child.communicate(input,std::move(options)));
}
bool retired(std::uint64_t id) {
#if defined(_WIN32)
    auto handle=OpenProcess(SYNCHRONIZE,FALSE,static_cast<DWORD>(id));
    if(!handle) return GetLastError()==ERROR_INVALID_PARAMETER;
    const auto done=WaitForSingleObject(handle,0)==WAIT_OBJECT_0; CloseHandle(handle); return done;
#else
    if(kill(static_cast<pid_t>(id),0)<0 && errno==ESRCH) return true;
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    proc_bsdinfo info{};
    return proc_pidinfo(static_cast<int>(id),PROC_PIDTBSDINFO,0,&info,sizeof(info))==sizeof(info) && info.pbi_status==SZOMB;
#elif defined(__linux__)
    // An orphan zombie is dead; its reaping belongs to the host's init.
    const auto path="/proc/"+std::to_string(id)+"/stat";
    FILE* file=std::fopen(path.c_str(),"r"); if(!file) return errno==ENOENT;
    std::array<char,1024> bytes{}; const auto size=std::fread(bytes.data(),1,bytes.size()-1,file); std::fclose(file);
    const std::string_view line(bytes.data(),size); const auto end=line.rfind(')');
    return end!=line.npos && end+2<line.size() && line[end+2]=='Z';
#else
    return false;
#endif
#endif
}
void wait_retired(std::uint64_t id) {
    for(int i=0;i<200 && !retired(id);++i) std::this_thread::sleep_for(5ms);
    check(retired(id),"owned descendant terminated");
}
std::uint64_t descendant(Process& child) {
    std::array<std::byte,64> bytes{};
    auto channel=child.output();
    check(bool(channel.wait(ii::io::Interest::readable,2s)),"tree announced descendant");
    auto read=channel.read(bytes); check(bool(read) && read->bytes>0,"descendant PID read");
    return std::stoull(std::string(reinterpret_cast<const char*>(bytes.data()),read->bytes));
}
void contracts(const std::string& fixture) {
    auto basic=run(launch(fixture,{"args","","quoted \" \\ ending\\","; $() & | < > 끝"}));
    check(basic.exit.reason==ExitReason::exited && basic.exit.code==0,"native successful exit");
    const std::vector<std::string> args{"","quoted \" \\ ending\\","; $() & | < > 끝"};
    std::string expected; for(const auto& a:args) expected+=std::to_string(a.size())+":"+a+"\n";
    check(text(basic.output)==expected,"empty arguments quoting Unicode and shell literals preserved");
    check(run(launch(fixture,{"exit","42"})).exit.code==42,"nonzero exit is an outcome");
    auto streams=run(launch(fixture,{"streams"}));
    check(streams.exit.code==7 && text(streams.output)=="out\n" && text(streams.error)=="err\n","independent stdout/stderr");
    auto merged=launch(fixture,{"streams"}); merged.error=ErrorOutput::merge_output;
    streams=run(merged); check(text(streams.output)=="out\nerr\n" && streams.error.empty(),"merged streams share ordered OS pipe");
    merged.output=Output::discard; streams=run(merged); check(streams.output.empty() && streams.exit.code==7,"discarded output");
    auto empty=launch(fixture,{"copy"}); empty.input=Input::closed;
    check(run(empty).output.empty(),"closed input delivers EOF");

    const auto parent=take(current_environment()); const auto parent_cwd=std::filesystem::current_path();
    const auto directory=parent_cwd/std::filesystem::path(u8"process fixture 작업 디렉터리");
    std::filesystem::create_directories(directory);
    auto env=launch(fixture,{"environment","IISDKCORE_PROCESS_TEST","IISDKCORE_PROCESS_EMPTY","PATH"});
    env.inherit_environment=false; env.working_directory=directory;
    env.environment={{"IISDKCORE_PROCESS_TEST","value with spaces 한글"},{"IISDKCORE_PROCESS_EMPTY",""},{"PATH",std::nullopt}};
    auto values=run(env);
    check(text(values.output)=="value with spaces 한글\n\n<unset>\n"+utf8(std::filesystem::canonical(directory)),"child Unicode environment and cwd");
    env.inherit_environment=true; env.environment["PATH"]=std::nullopt;
    check(text(run(env).output).find("<unset>\n")!=std::string::npos,"inherited variable explicitly removed");
    check(take(current_environment())==parent && std::filesystem::current_path()==parent_cwd,"parent environment/cwd preserved");
    auto path=launch(std::filesystem::path(fixture).filename().string(),{"echo","resolved"});
    path.inherit_environment=false; path.environment["PATH"]=utf8(std::filesystem::path(fixture).parent_path()); path.working_directory=directory;
    check(text(run(path).output)=="resolved","executable lookup uses child PATH");
    path.environment["PATH"]=utf8(std::filesystem::relative(std::filesystem::path(fixture).parent_path(),directory));
    check(text(run(path).output)=="resolved","relative PATH components use child cwd");
    path.arguments[0]=utf8(std::filesystem::relative(std::filesystem::path(fixture),directory));
    check(text(run(path).output)=="resolved","relative executable resolved in child cwd");
    std::filesystem::remove(directory);

    ii::core::Bytes input(1024*1024+13); for(std::size_t i=0;i<input.size();++i) input[i]=std::byte(i%253);
    auto large=run(launch(fixture,{"duplex"}),input);
    check(large.exit.code==0 && large.input_written==input.size() && !large.input_closed_early,"duplex pumps stdin beyond pipe capacity");
    check(large.output.size()==1024*1024+input.size() && large.error.size()==1024*1024,"both large output streams drained");
    check(std::equal(input.begin(),input.end(),large.output.begin()+1024*1024),"binary stdin echoed without corruption");
    check(!large.output_truncated && !large.error_truncated,"normal bounded capture");
    std::size_t seen_out=0,seen_err=0;
    CommunicationOptions bounded; bounded.output_limit=31; bounded.error_limit=17;
    bounded.receive=[&](Stream stream,ii::core::ByteView chunk) { (stream==Stream::output?seen_out:seen_err)+=chunk.size(); };
    auto limited=run(launch(fixture,{"duplex"}),input,bounded);
    check(limited.output.size()==31 && limited.error.size()==17 && limited.output_truncated && limited.error_truncated,"capture bounded without deadlock");
    check(seen_out==large.output.size() && seen_err==large.error.size(),"sink receives every byte despite capture limits");
    auto early=run(launch(fixture,{"early-input"}),input);
    check(early.exit.code==0 && early.input_closed_early && early.input_written<input.size(),"broken stdin cannot signal SIGPIPE to host");
    check(text(early.output)=="ready","drain after early input closure");

    auto sleeping=take(Process::start(launch(fixture,{"sleep"})));
    check(take(sleeping.wait_for(15ms))==std::nullopt,"wait timeout leaves child running");
    auto stopped=take(sleeping.cancel()); check(stopped.reason==ExitReason::cancelled,"explicit cancel");
    check(take(sleeping.cancel()).reason==stopped.reason,"cancel is idempotent");
    check(take(sleeping.poll())->reason==stopped.reason,"stable reaped result");
    check(!sleeping.wait_for(-1ms),"negative wait rejected");
    CommunicationOptions timed; timed.deadline=ii::core::Deadline::after(40ms);
    auto timeout_child=take(Process::start(launch(fixture,{"sleep"})));
    auto timeout=take(timeout_child.communicate({},timed)); check(timeout.exit.reason==ExitReason::timed_out,"deadline terminates child");
    std::stop_source stop; CommunicationOptions cancellation; cancellation.stop=stop.get_token();
    auto cancellable=take(Process::start(launch(fixture,{"sleep"})));
    std::thread request([&] { std::this_thread::sleep_for(25ms); stop.request_stop(); });
    auto cancelled=cancellable.communicate({},cancellation);
    request.join();
    check(take(std::move(cancelled)).exit.reason==ExitReason::cancelled,"stop token cancels native child");
    stop.request_stop(); auto pre=run(launch(fixture,{"sleep"}),{},cancellation);
    check(pre.exit.reason==ExitReason::cancelled,"already stopped communication retires launched child");

    auto tree=take(Process::start(launch(fixture,{"tree"}))); const auto leaf=descendant(tree);
    take(tree.cancel()); wait_retired(leaf);
    const auto root=tree.id();
#if !defined(_WIN32)
    int status=0; check(waitpid(static_cast<pid_t>(root),&status,WNOHANG)==-1 && errno==ECHILD,"direct child reaped exactly once");
#endif
    auto natural=run(launch(fixture,{"tree-exit"}));
    check(natural.exit.code==0,"root exit does not hang on descendant pipe owner"); wait_retired(std::stoull(text(natural.output)));
    std::uint64_t removed_id=0,removed_leaf=0; ii::io::Channel alias;
    {
        auto removed=take(Process::start(launch(fixture,{"tree"}))); removed_id=removed.id(); removed_leaf=descendant(removed); alias=removed.output();
    }
    wait_retired(removed_id); wait_retired(removed_leaf);
    std::array<std::byte,1> one{}; check(!alias.read(one) && alias.read(one).error().code==ii::io::ErrorCode::closed,"owner removal closes channel aliases");
    auto replacement=take(Process::start(launch(fixture,{"sleep"}))); auto previous=replacement.id();
    replacement=take(Process::start(launch(fixture,{"echo","moved"}))); wait_retired(previous);
    check(text(take(replacement.communicate()).output)=="moved","move assignment retires previous tree");

    auto throwing=take(Process::start(launch(fixture,{"duplex"}))); const auto throwing_id=throwing.id();
    CommunicationOptions callback; callback.receive=[](Stream,ii::core::ByteView) { throw std::runtime_error("sink failure"); };
    auto failure=throwing.communicate({},callback);
    check(!failure && failure.error().cause,"sink exception reported with cause"); wait_retired(throwing_id);
    auto concurrent=take(Process::start(launch(fixture,{"duplex"})));
    std::promise<void> entered,release;
    auto entered_future=entered.get_future(); auto released=release.get_future().share();
    std::atomic<bool> first=true;
    CommunicationOptions held;
    held.receive=[&](Stream,ii::core::ByteView) { if(first.exchange(false)) { entered.set_value(); released.wait(); } };
    auto communicating=std::async(std::launch::async,[&] { return concurrent.communicate({},held); });
    const bool admitted=entered_future.wait_for(2s)==std::future_status::ready;
    auto competing=concurrent.communicate();
    auto concurrent_exit=concurrent.cancel();
    release.set_value();
    auto communicated=communicating.get();
    check(admitted && !competing && competing.error().code==ii::core::ErrorCode::busy,"concurrent communication rejected without consuming pipes");
    check(concurrent_exit && communicated && communicated->exit.reason==ExitReason::cancelled,"control cancellation works while sink is active");

    auto resources=[] {
#if defined(_WIN32)
        DWORD count=0; GetProcessHandleCount(GetCurrentProcess(),&count); return static_cast<int>(count);
#else
        int count=0; for(int fd=0;fd<1024;++fd) if(fcntl(fd,F_GETFD)>=0) ++count; return count;
#endif
    };
    const auto before=resources();
    for(int i=0;i<20;++i) {
        check(run(launch(fixture,{"echo","reuse"})).exit.code==0,"repeated native launch");
        auto rejected=launch(fixture+"-missing",{}); check(!Process::start(rejected),"repeated spawn failure cleanup");
    }
    check(resources()==before,"native handles/descriptors returned after success and spawn failure");

    auto invalid=launch(fixture,{"echo"}); invalid.arguments[0]+="-missing";
    check(!Process::start(invalid),"spawn failure distinct from child exit");
    invalid=launch(fixture,{"echo"}); invalid.working_directory="process-missing-directory";
    check(!Process::start(invalid),"invalid child cwd rejected");
    invalid=launch(fixture,{"echo",std::string("a\0b",3)}); check(!Process::start(invalid),"NUL argument rejected");
    invalid=launch(fixture,{"echo",std::string(1,char(0xff))}); check(!Process::start(invalid),"invalid UTF-8 rejected");
    invalid=launch(fixture,{"echo"}); invalid.environment["A=B"]="value"; check(!Process::start(invalid),"invalid environment key rejected");
    invalid=launch(fixture,{"echo"}); invalid.environment["A"]=std::string("x\0y",3); check(!Process::start(invalid),"NUL environment value rejected");
    invalid=launch(fixture,{"echo"}); invalid.input=static_cast<Input>(99); check(!Process::start(invalid),"invalid pipe policy rejected");
    check(!Process::start(Launch{}) && !Process{}.poll() && !Process{}.cancel(),"empty owner/launch fail explicitly");
    check(run(launch(fixture,{"closed-stdio"})).exit.code==0,"spawn works when host standard handles are closed");
#if !defined(_WIN32)
    auto signaled=run(launch(fixture,{"signal"})); check(signaled.exit.reason==ExitReason::signaled && signaled.exit.signal==SIGTERM,"signal exit retained");
    const int sentinel=fcntl(STDOUT_FILENO,F_DUPFD,512); check(sentinel>=0,"unrelated inheritable descriptor fixture");
    auto isolation=run(launch(fixture,{"inherited-handle",std::to_string(sentinel)})); close(sentinel);
    check(isolation.exit.code==0,"unrelated non-CLOEXEC descriptor not inherited");
#else
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE}; auto sentinel=CreateEventW(&security,TRUE,FALSE,nullptr);
    check(bool(sentinel),"unrelated inheritable handle fixture");
    auto isolation=run(launch(fixture,{"inherited-handle",std::to_string(reinterpret_cast<std::uintptr_t>(sentinel))})); CloseHandle(sentinel);
    check(isolation.exit.code==0,"unrelated inheritable handle not inherited");
#endif
    // A caller can use an EventLoop instead of communicate; pipe kinds retain
    // the existing bounded readiness/completion contract.
    auto piped=take(Process::start(launch(fixture,{"echo","loop"}))); (void)piped.input().close();
    ii::runtime::EventLoop loop(4);
    bool delivered=false;
    ii::io::Buffer buffer(4);
    auto watched=loop.read(piped.output(),ii::io::Read{buffer,0,4,2s},[&](ii::io::IOResult result) {
        check(bool(result) && result->transfer.bytes==4 &&
            std::string(reinterpret_cast<const char*>(buffer.bytes().data()),4)=="loop","event loop process pipe transfer"); delivered=true;
    });
    check(bool(watched),"process channel accepted by EventLoop");
    const auto delivery=ii::core::Deadline::after(2s);
    while(!delivered && !delivery.expired()) { (void)loop.poll(); std::this_thread::sleep_for(2ms); }
    check(delivered,"process readiness delivered");
    check(take(piped.wait_for(2s)).has_value(),"event loop child reaped");
}
}
int main(int argc,char** argv) {
    try {
        check(argc==2,"process fixture path required");
        if(!capabilities().launch) {
            check(!capabilities().pipes && !capabilities().working_directory && !capabilities().owned_tree,"unsupported capability matrix");
            auto unsupported=Process::start(Launch{});
            check(!unsupported && unsupported.error().code==ii::core::ErrorCode::unsupported,"no interpreter or host bridge fallback");
#if defined(__EMSCRIPTEN__)
            check(!current_environment(),"WASM cannot read a host process environment");
#endif
        } else contracts(utf8(std::filesystem::absolute(argv[1])));
        std::cout<<checks<<" native process checks passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<"\n"; return 1; }
}
