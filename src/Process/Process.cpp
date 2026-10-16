#include "Process.h"
#include "../IO/NativePipe.h"
#include "../Core/Text/Utf8.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#if defined(__APPLE__)
#include <AvailabilityMacros.h>
#endif
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(IISDKCORE_PROCESS_POSIX)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc.h>
#endif
#endif
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
extern char** environ;
#endif

namespace ii::process {
namespace {
using core::ErrorCode;
auto failure(ErrorCode code, std::string text) { return core::fail(code,std::move(text)); }
auto native_error(std::string operation, int number) {
#if defined(_WIN32)
    std::error_code error(number,std::system_category());
#else
    std::error_code error(number,std::generic_category());
#endif
    auto code=ErrorCode::failed;
    if (error==std::errc::no_such_file_or_directory || error==std::errc::not_a_directory) code=ErrorCode::not_found;
    if (error==std::errc::invalid_argument) code=ErrorCode::invalid_argument;
    return core::fail(code,operation+": "+error.message(),std::make_exception_ptr(std::system_error(error,operation)));
}
auto pipe_error(std::string operation, const io::Error& error) {
    return core::fail(error.code==io::ErrorCode::closed ? ErrorCode::closed : ErrorCode::failed,
        operation+": "+error.native.message(),std::make_exception_ptr(std::system_error(error.native,operation)));
}
bool text_valid(std::string_view text) { return text.find('\0')==text.npos && core::valid_utf8(text); }
#if defined(_WIN32)
std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    if (text.size()>INT_MAX) throw std::invalid_argument("UTF-8 input too large");
    const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if (!count) throw std::invalid_argument("Invalid UTF-8");
    std::wstring value(count,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),value.data(),count);
    return value;
}
std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const auto count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if (!count) throw std::invalid_argument("Invalid UTF-16 environment");
    std::string value(count,'\0');
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),value.data(),count,nullptr,nullptr);
    return value;
}
bool same_key(std::string_view a,std::string_view b) {
    auto left=widen(a),right=widen(b);
    return CompareStringOrdinal(left.data(),static_cast<int>(left.size()),right.data(),static_cast<int>(right.size()),TRUE)==CSTR_EQUAL;
}
std::wstring quote(std::wstring_view text) {
    std::wstring value=L"\"";
    std::size_t slashes=0;
    for (auto c:text) {
        if (c==L'\\') { ++slashes; continue; }
        value.append(c==L'"'?2*slashes+1:slashes,L'\\'); value+=c; slashes=0;
    }
    value.append(2*slashes,L'\\'); return value+L'"';
}
struct Handle final {
    HANDLE value=INVALID_HANDLE_VALUE;
    Handle()=default;
    explicit Handle(HANDLE h):value(h) {}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other) noexcept:value(std::exchange(other.value,INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept { if (this!=&other) { reset(); value=std::exchange(other.value,INVALID_HANDLE_VALUE); } return *this; }
    bool valid() const { return value && value!=INVALID_HANDLE_VALUE; }
    HANDLE release() { return std::exchange(value,INVALID_HANDLE_VALUE); }
    void reset() { if (valid()) CloseHandle(value); value=INVALID_HANDLE_VALUE; }
    ~Handle() { reset(); }
};
#elif defined(IISDKCORE_PROCESS_POSIX)
struct Descriptor final {
    int value=-1;
    Descriptor()=default;
    explicit Descriptor(int fd):value(fd) {}
    Descriptor(const Descriptor&)=delete;
    Descriptor& operator=(const Descriptor&)=delete;
    Descriptor(Descriptor&& other) noexcept:value(std::exchange(other.value,-1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept { if(this!=&other) { reset(); value=std::exchange(other.value,-1); } return *this; }
    void reset() { if(value>=0) ::close(value); value=-1; }
    int release() { return std::exchange(value,-1); }
    ~Descriptor() { reset(); }
};
Result<Descriptor> reserve_descriptor(int fd) {
    Descriptor original(fd);
    if (fd<0) return native_error("open process endpoint",errno);
    const auto safe=fcntl(fd,F_DUPFD_CLOEXEC,3);
    if (safe<0) return native_error("reserve process endpoint",errno);
    return Descriptor(safe);
}
#if defined(__APPLE__)
bool dead_group(pid_t group) {
    // Darwin's killpg can return EPERM when its only members are zombies.
    // Verify that condition; never disguise a live member's permission error.
    errno=0;
    const auto expected=proc_listpgrppids(group,nullptr,0);
    if(expected<0 || (expected==0 && errno)) return false;
    std::vector<pid_t> members(static_cast<std::size_t>(expected)+32);
    errno=0;
    const auto count=proc_listpgrppids(group,members.data(),static_cast<int>(members.size()*sizeof(pid_t)));
    if(count<0 || (count==0 && errno) || static_cast<std::size_t>(count)>=members.size()) return false;
    for(int i=0;i<count;++i) {
        proc_bsdinfo info{}; errno=0;
        if(proc_pidinfo(members[i],PROC_PIDTBSDINFO,0,&info,sizeof(info))!=sizeof(info)) {
            if(errno==ESRCH) continue;
            return false;
        }
        if(info.pbi_status!=SZOMB) return false;
    }
    return true;
}
#endif
#endif
Result<Environment> make_environment(const Launch& launch) {
    Environment values;
    if (launch.inherit_environment) {
        auto inherited=current_environment(); if(!inherited) return std::unexpected(inherited.error());
        values=std::move(*inherited);
    }
    for (const auto& [key,value]:launch.environment) {
#if defined(_WIN32)
        std::erase_if(values,[&](const auto& item) { return same_key(item.first,key); });
#endif
        if (value) values.insert_or_assign(key,*value); else values.erase(key);
    }
    return values;
}
}

Capabilities capabilities() noexcept {
#if defined(_WIN32) || defined(IISDKCORE_PROCESS_POSIX)
#if defined(__APPLE__)
    if (__builtin_available(macOS 10.15, *)) return {true,true,true,true};
    return {false,false,false,false};
#else
    return {true,true,true,true};
#endif
#else
    return {false,false,false,false};
#endif
}
Result<Environment> current_environment() {
    try {
        Environment values;
#if defined(_WIN32)
        struct Block final { wchar_t* value=GetEnvironmentStringsW(); ~Block() { if(value) FreeEnvironmentStringsW(value); } } block;
        if (!block.value) return native_error("snapshot environment",static_cast<int>(GetLastError()));
        for (auto entry=block.value; *entry;) {
            std::wstring_view text(entry); const auto split=text.find(L'=',text.front()==L'='?1:0);
            if(split!=text.npos) values.insert_or_assign(narrow(text.substr(0,split)),narrow(text.substr(split+1)));
            entry+=text.size()+1;
        }
#elif !defined(__EMSCRIPTEN__)
        if (environ) for(auto entry=environ; *entry; ++entry) {
            std::string_view text(*entry); const auto split=text.find('=');
            if(split!=text.npos) values.insert_or_assign(std::string(text.substr(0,split)),std::string(text.substr(split+1)));
        }
#else
        return failure(ErrorCode::unsupported,"Host process environment is unavailable on this target");
#endif
        return values;
    } catch (...) { return core::fail(ErrorCode::failed,"Cannot snapshot environment",std::current_exception()); }
}

struct Process::State final {
    std::mutex control,communication;
    io::Channel input,output,error;
    std::uint64_t identifier=0;
    std::optional<Exit> exit;
#if defined(_WIN32)
    Handle process,job;
#elif defined(IISDKCORE_PROCESS_POSIX)
    pid_t pid=-1;
#endif
    Result<std::optional<Exit>> observe(std::optional<ExitReason> terminate={}) {
        std::lock_guard lock(control);
        if (exit) return exit;
#if defined(_WIN32)
        if (!process.valid()) return failure(ErrorCode::closed,"No owned process");
        DWORD state=WaitForSingleObject(process.value,0);
        if (state==WAIT_FAILED) return native_error("wait process",static_cast<int>(GetLastError()));
        if (state==WAIT_TIMEOUT && !terminate) return std::optional<Exit>{};
        // The root handle pins identity while its job is retired, including on
        // normal exit. A surviving descendant must not retain stdio forever.
        if (!TerminateJobObject(job.value,1)) return native_error("terminate process tree",static_cast<int>(GetLastError()));
        if (WaitForSingleObject(process.value,INFINITE)==WAIT_FAILED) return native_error("reap process",static_cast<int>(GetLastError()));
        DWORD code=0;
        if (!GetExitCodeProcess(process.value,&code)) return native_error("process exit",static_cast<int>(GetLastError()));
        exit=Exit{terminate.value_or(ExitReason::exited),code,0};
        process.reset(); job.reset();
#elif defined(IISDKCORE_PROCESS_POSIX)
        if (pid<=0) return failure(ErrorCode::closed,"No owned process");
        siginfo_t info{};
        int result;
        do { result=waitid(P_PID,static_cast<id_t>(pid),&info,WEXITED|WNOHANG|WNOWAIT); } while(result<0 && errno==EINTR);
        if(result<0) {
            // A host that reaps our child (SIGCHLD=SIG_IGN, SA_NOCLDWAIT, or a
            // competing waitpid) has invalidated identity. Never signal reuse.
            pid=-1;
            return native_error("observe owned child (host must not reap it)",errno);
        }
        if(info.si_pid==0 && !terminate) return std::optional<Exit>{};
        if (::kill(-pid,SIGKILL)!=0 && errno!=ESRCH) {
            const auto number=errno;
#if defined(__APPLE__)
            if(number!=EPERM || info.si_pid==0 || !dead_group(pid))
#endif
                return native_error("terminate process group",number);
        }
        // The root itself remains owned even if it changed process group.
        if(info.si_pid==0 && ::kill(pid,SIGKILL)!=0 && errno!=ESRCH) return native_error("terminate process root",errno);
        int status=0;
        pid_t waited;
        do { waited=waitpid(pid,&status,0); } while(waited<0 && errno==EINTR);
        if(waited<0) { pid=-1; return native_error("reap owned child",errno); }
        pid=-1;
        exit=Exit{terminate.value_or(WIFSIGNALED(status)?ExitReason::signaled:ExitReason::exited),
            WIFEXITED(status)?static_cast<std::uint32_t>(WEXITSTATUS(status)):0,
            WIFSIGNALED(status)?WTERMSIG(status):0};
#else
        (void)terminate;
        return failure(ErrorCode::unsupported,"Native process launch is unavailable on this target");
#endif
        return exit;
    }
    ~State() {
        if(identifier) try { (void)observe(ExitReason::owner_removed); } catch(...) {}
        (void)input.close(); (void)output.close(); (void)error.close();
    }
};
Process::Process() noexcept=default;
Process::~Process()=default;
Process::Process(Process&&) noexcept=default;
Process& Process::operator=(Process&&) noexcept=default;
std::uint64_t Process::id() const noexcept { return state_?state_->identifier:0; }
io::Channel Process::input() const { return state_?state_->input:io::Channel{}; }
io::Channel Process::output() const { return state_?state_->output:io::Channel{}; }
io::Channel Process::error() const { return state_?state_->error:io::Channel{}; }
Result<std::optional<Exit>> Process::poll() {
    if(!state_) return failure(ErrorCode::closed,"No owned process");
    return state_->observe();
}
Result<std::optional<Exit>> Process::wait_for(std::chrono::milliseconds timeout) {
    if(timeout.count()<0) return failure(ErrorCode::invalid_argument,"Negative process wait");
    const auto deadline=core::Deadline::after(timeout);
    for(;;) {
        auto result=poll(); if(!result || *result || deadline.expired()) return result;
        std::this_thread::sleep_for(std::min(deadline.remaining(),std::chrono::duration_cast<core::Clock::duration>(std::chrono::milliseconds(5))));
    }
}
Result<Exit> Process::cancel() {
    if(!state_) return failure(ErrorCode::closed,"No owned process");
    auto result=state_->observe(ExitReason::cancelled);
    if(!result) return std::unexpected(result.error());
    return **result;
}

Result<Process> Process::start(const Launch& launch) {
    if(!capabilities().launch) return failure(ErrorCode::unsupported,"Native process launch is unavailable on this target");
    if(launch.arguments.empty() || launch.arguments.front().empty()) return failure(ErrorCode::invalid_argument,"Executable required");
    for(const auto& argument:launch.arguments) if(!text_valid(argument)) return failure(ErrorCode::invalid_argument,"Process arguments require UTF-8 without NUL");
    for(const auto& [key,value]:launch.environment)
        if(key.empty() || key.find('=')!=key.npos || !text_valid(key) || (value && !text_valid(*value)))
            return failure(ErrorCode::invalid_argument,"Invalid child environment entry");
    if(launch.working_directory.native().find(std::filesystem::path::value_type{})!=std::filesystem::path::string_type::npos)
        return failure(ErrorCode::invalid_argument,"NUL in child working directory");
    if((launch.input!=Input::pipe && launch.input!=Input::closed && launch.input!=Input::inherit) ||
       (launch.output!=Output::pipe && launch.output!=Output::discard && launch.output!=Output::inherit) ||
       (launch.error!=ErrorOutput::pipe && launch.error!=ErrorOutput::merge_output && launch.error!=ErrorOutput::discard && launch.error!=ErrorOutput::inherit))
        return failure(ErrorCode::invalid_argument,"Invalid standard stream mode");
    try {
        auto environment=make_environment(launch); if(!environment) return std::unexpected(environment.error());
        Process owner; owner.state_=std::make_unique<State>(); auto& state=*owner.state_;
#if defined(_WIN32)
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
        std::array<Handle,3> children;
        auto prepare=[&](int index,bool piped,bool inherited) -> Result<void> {
            const bool read=index==0;
            if(piped) {
                static std::atomic<std::uint64_t> sequence=0;
                const auto name=L"\\\\.\\pipe\\iiSDKCore-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++sequence);
                Handle parent(CreateNamedPipeW(name.c_str(),(read?PIPE_ACCESS_OUTBOUND:PIPE_ACCESS_INBOUND)|FILE_FLAG_FIRST_PIPE_INSTANCE,
                    PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_NOWAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr));
                if(!parent.valid()) return native_error("create process pipe",static_cast<int>(GetLastError()));
                children[index]=Handle(CreateFileW(name.c_str(),read?GENERIC_READ:GENERIC_WRITE,0,&security,OPEN_EXISTING,0,nullptr));
                if(!children[index].valid()) return native_error("connect process pipe",static_cast<int>(GetLastError()));
                if(!ConnectNamedPipe(parent.value,nullptr) && GetLastError()!=ERROR_PIPE_CONNECTED)
                    return native_error("connect parent pipe",static_cast<int>(GetLastError()));
                auto channel=io::NativePipe::adopt(reinterpret_cast<std::intptr_t>(parent.release()),read?io::Access::write:io::Access::read);
                if(!channel) return pipe_error("adopt process pipe",channel.error());
                (index==0?state.input:index==1?state.output:state.error)=*channel;
            } else if(inherited) {
                const DWORD which=index==0?STD_INPUT_HANDLE:index==1?STD_OUTPUT_HANDLE:STD_ERROR_HANDLE;
                HANDLE duplicate=nullptr;
                if(!DuplicateHandle(GetCurrentProcess(),GetStdHandle(which),GetCurrentProcess(),&duplicate,0,TRUE,DUPLICATE_SAME_ACCESS))
                    return native_error("inherit standard stream",static_cast<int>(GetLastError()));
                children[index]=Handle(duplicate);
            } else {
                children[index]=Handle(CreateFileW(L"NUL",read?GENERIC_READ:GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr));
                if(!children[index].valid()) return native_error("open null stream",static_cast<int>(GetLastError()));
            }
            return {};
        };
        auto prepared=prepare(0,launch.input==Input::pipe,launch.input==Input::inherit); if(!prepared) return std::unexpected(prepared.error());
        prepared=prepare(1,launch.output==Output::pipe,launch.output==Output::inherit); if(!prepared) return std::unexpected(prepared.error());
        if(launch.error!=ErrorOutput::merge_output) {
            prepared=prepare(2,launch.error==ErrorOutput::pipe,launch.error==ErrorOutput::inherit); if(!prepared) return std::unexpected(prepared.error());
        }
        state.job=Handle(CreateJobObjectW(nullptr,nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!state.job.valid() || !SetInformationJobObject(state.job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))
            return native_error("create owned job",static_cast<int>(GetLastError()));
        std::vector<std::pair<std::wstring,std::wstring>> wide_environment;
        std::wstring search;
        for(const auto& [key,value]:*environment) {
            wide_environment.emplace_back(widen(key),widen(value));
            if(same_key(key,"PATH")) search=widen(value);
        }
        std::sort(wide_environment.begin(),wide_environment.end(),[](const auto& a,const auto& b) {
            return CompareStringOrdinal(a.first.data(),static_cast<int>(a.first.size()),b.first.data(),static_cast<int>(b.first.size()),TRUE)==CSTR_LESS_THAN;
        });
        std::vector<wchar_t> block;
        for(const auto& [key,value]:wide_environment) {
            block.insert(block.end(),key.begin(),key.end()); block.push_back(L'=');
            block.insert(block.end(),value.begin(),value.end()); block.push_back(L'\0');
        }
        block.push_back(L'\0'); if(block.size()==1) block.push_back(L'\0');
        const auto cwd=launch.working_directory.empty()?std::filesystem::current_path():std::filesystem::absolute(launch.working_directory);
        auto executable=std::filesystem::path(widen(launch.arguments.front()));
        if(executable.has_parent_path()) { if(executable.is_relative()) executable=cwd/executable; }
        else {
            // Explicit search uses the child's PATH and cwd, not the parent's
            // implicit CreateProcess search or a command shell.
            std::wstring paths=cwd.native();
            std::size_t begin=0;
            while(begin<search.size()) {
                const auto end=search.find(L';',begin);
                auto entry=search.substr(begin,end==search.npos?search.size()-begin:end-begin);
                if(entry.size()>=2 && entry.front()==L'"' && entry.back()==L'"') entry=entry.substr(1,entry.size()-2);
                auto directory=std::filesystem::path(entry);
                if(directory.is_relative()) directory=cwd/directory;
                paths+=L";"+directory.native();
                if(end==search.npos) break;
                begin=end+1;
            }
            const auto required=SearchPathW(paths.c_str(),executable.c_str(),L".exe",0,nullptr,nullptr);
            if(!required) return native_error("resolve executable",static_cast<int>(GetLastError()));
            std::wstring found(required+1,L'\0');
            const auto count=SearchPathW(paths.c_str(),executable.c_str(),L".exe",static_cast<DWORD>(found.size()),found.data(),nullptr);
            if(!count || count>=found.size()) return failure(ErrorCode::failed,"Executable lookup changed");
            found.resize(count); executable=found;
        }
        std::wstring command;
        for(const auto& argument:launch.arguments) { if(!command.empty()) command+=L' '; command+=quote(widen(argument)); }
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb=sizeof(startup); startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput=children[0].value; startup.StartupInfo.hStdOutput=children[1].value;
        startup.StartupInfo.hStdError=launch.error==ErrorOutput::merge_output?children[1].value:children[2].value;
        SIZE_T bytes=0; InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
        std::vector<std::byte> attributes(bytes);
        startup.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if(!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&bytes)) return native_error("initialize handle whitelist",static_cast<int>(GetLastError()));
        struct Attributes final { LPPROC_THREAD_ATTRIBUTE_LIST value; ~Attributes() { DeleteProcThreadAttributeList(value); } } cleanup{startup.lpAttributeList};
        std::vector<HANDLE> inherited{children[0].value,children[1].value};
        if(launch.error!=ErrorOutput::merge_output) inherited.push_back(children[2].value);
        if(!UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited.data(),inherited.size()*sizeof(HANDLE),nullptr,nullptr))
            return native_error("configure handle whitelist",static_cast<int>(GetLastError()));
        PROCESS_INFORMATION information{};
        if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,
            block.data(),cwd.c_str(),&startup.StartupInfo,&information)) return native_error("spawn executable",static_cast<int>(GetLastError()));
        state.process=Handle(information.hProcess); Handle thread(information.hThread); state.identifier=information.dwProcessId;
        if(!AssignProcessToJobObject(state.job.value,state.process.value)) {
            const auto error=GetLastError(); TerminateProcess(state.process.value,1); WaitForSingleObject(state.process.value,INFINITE); state.process.reset();
            return native_error("assign owned job",static_cast<int>(error));
        }
        if(ResumeThread(thread.value)==static_cast<DWORD>(-1)) return native_error("resume child",static_cast<int>(GetLastError()));
#elif defined(IISDKCORE_PROCESS_POSIX)
        std::array<Descriptor,3> children;
        auto prepare=[&](int index,bool piped,bool inherited) -> Result<void> {
            const bool read=index==0;
            if(piped) {
                int pair[2];
#if defined(__linux__)
                if(::pipe2(pair,O_CLOEXEC)!=0) return native_error("create process pipe",errno);
#else
                if(::pipe(pair)!=0) return native_error("create process pipe",errno);
#endif
                Descriptor raw_read(pair[0]),raw_write(pair[1]);
                auto first=reserve_descriptor(raw_read.release()),second=reserve_descriptor(raw_write.release());
                if(!first) return std::unexpected(first.error()); if(!second) return std::unexpected(second.error());
                Descriptor parent=read?std::move(*second):std::move(*first);
                children[index]=read?std::move(*first):std::move(*second);
                auto channel=io::NativePipe::adopt(parent.release(),read?io::Access::write:io::Access::read);
                if(!channel) return pipe_error("adopt process pipe",channel.error());
                (index==0?state.input:index==1?state.output:state.error)=*channel;
            } else {
                auto descriptor=reserve_descriptor(inherited?fcntl(index,F_DUPFD_CLOEXEC,3): ::open("/dev/null",(read?O_RDONLY:O_WRONLY)|O_CLOEXEC));
                if(!descriptor) return std::unexpected(descriptor.error());
                children[index]=std::move(*descriptor);
            }
            return {};
        };
        auto prepared=prepare(0,launch.input==Input::pipe,launch.input==Input::inherit); if(!prepared) return std::unexpected(prepared.error());
        prepared=prepare(1,launch.output==Output::pipe,launch.output==Output::inherit); if(!prepared) return std::unexpected(prepared.error());
        if(launch.error!=ErrorOutput::merge_output) {
            prepared=prepare(2,launch.error==ErrorOutput::pipe,launch.error==ErrorOutput::inherit); if(!prepared) return std::unexpected(prepared.error());
        }
        std::vector<std::string> entries;
        for(const auto& [key,value]:*environment) entries.push_back(key+"="+value);
        std::vector<char*> envp,argv;
        for(auto& entry:entries) envp.push_back(entry.data()); envp.push_back(nullptr);
        for(const auto& argument:launch.arguments) argv.push_back(const_cast<char*>(argument.c_str())); argv.push_back(nullptr);
        std::vector<std::string> executables;
        if(launch.arguments.front().find('/')!=std::string::npos) executables.push_back(launch.arguments.front());
        else {
            auto path=environment->find("PATH");
            // Fixed native default when PATH is absent; an explicitly empty
            // PATH searches only the child's cwd, matching POSIX path entries.
            const std::string_view search=path==environment->end()?std::string_view("/usr/bin:/bin"):std::string_view(path->second);
            std::size_t begin=0;
            do {
                const auto end=search.find(':',begin); auto directory=search.substr(begin,end==search.npos?search.size()-begin:end-begin);
                executables.push_back(directory.empty()?launch.arguments.front():std::string(directory)+"/"+launch.arguments.front());
                if(end==search.npos) break; begin=end+1;
            } while(true);
        }
        struct Actions final { posix_spawn_file_actions_t value; bool ready=false; ~Actions() { if(ready) posix_spawn_file_actions_destroy(&value); } } actions;
        struct Attributes final { posix_spawnattr_t value; bool ready=false; ~Attributes() { if(ready) posix_spawnattr_destroy(&value); } } attributes;
        int number=posix_spawn_file_actions_init(&actions.value); if(number) return native_error("initialize spawn actions",number); actions.ready=true;
        number=posix_spawnattr_init(&attributes.value); if(number) return native_error("initialize spawn attributes",number); attributes.ready=true;
        for(int i=0;i<3 && !number;++i) number=posix_spawn_file_actions_adddup2(&actions.value,children[i==2 && launch.error==ErrorOutput::merge_output?1:i].value,i);
        // Keep the 10.15 entry point for older deployment targets. New SDKs
        // deprecate its spelling in favour of the macOS 26-only POSIX name.
#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        if(!number && !launch.working_directory.empty()) number=posix_spawn_file_actions_addchdir_np(&actions.value,launch.working_directory.c_str());
#if defined(__APPLE__)
#pragma clang diagnostic pop
#endif
#if defined(IISDKCORE_PROCESS_CLOSEFROM)
        if(!number) number=posix_spawn_file_actions_addclosefrom_np(&actions.value,3);
        constexpr short isolation=0;
#else
        constexpr short isolation=POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
        sigset_t empty{},defaults{}; sigemptyset(&empty); sigemptyset(&defaults); sigaddset(&defaults,SIGPIPE);
        if(!number) number=posix_spawnattr_setsigmask(&attributes.value,&empty);
        if(!number) number=posix_spawnattr_setsigdefault(&attributes.value,&defaults);
        if(!number) number=posix_spawnattr_setpgroup(&attributes.value,0);
        if(!number) number=posix_spawnattr_setflags(&attributes.value,isolation|POSIX_SPAWN_SETPGROUP|POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF);
        if(number) return native_error("configure native spawn",number);
        int denied=0;
        for(const auto& executable:executables) {
            pid_t pid=-1;
            number=posix_spawn(&pid,executable.c_str(),&actions.value,&attributes.value,argv.data(),envp.data());
            if(!number) { state.pid=pid; state.identifier=static_cast<std::uint64_t>(pid); break; }
            if(number==EACCES) { denied=number; continue; }
            if(number!=ENOENT && number!=ENOTDIR) break;
        }
        if(number) return native_error("spawn "+launch.arguments.front(),denied && (number==ENOENT || number==ENOTDIR)?denied:number);
#endif
        return owner;
    } catch (...) { return core::fail(ErrorCode::failed,"Cannot launch native process",std::current_exception()); }
}

Result<Communication> Process::communicate(core::ByteView bytes,CommunicationOptions options) {
    if(!state_) return failure(ErrorCode::closed,"No owned process");
    std::unique_lock lock(state_->communication,std::try_to_lock);
    if(!lock) return failure(ErrorCode::busy,"Pipe communication already active");
    if(!bytes.empty() && !state_->input.capabilities().writable) return failure(ErrorCode::invalid_argument,"Input requires a piped standard input");
    Communication result;
    bool writing=state_->input.capabilities().writable;
    bool output=state_->output.capabilities().readable,error=state_->error.capabilities().readable;
    bool completed=false;
    core::Deadline drain;
    auto abort=[&](core::Error error_value) -> Result<Communication> {
        (void)state_->observe(ExitReason::cancelled);
        (void)state_->input.close(); (void)state_->output.close(); (void)state_->error.close();
        return std::unexpected(std::move(error_value));
    };
    try {
        std::array<std::byte,16384> buffer;
        while(true) {
            bool progress=false;
            if(!completed) {
                std::optional<ExitReason> stop;
                if(options.stop.stop_requested()) stop=ExitReason::cancelled;
                else if(options.deadline.expired()) stop=ExitReason::timed_out;
                auto status=state_->observe(stop);
                if(!status) return abort(status.error());
                if(*status) { result.exit=**status; completed=true; drain=core::Deadline::after(std::chrono::seconds(2)); }
            }
            if(writing) {
                if(result.input_written==bytes.size() || completed) {
                    result.input_closed_early=result.input_written<bytes.size();
                    auto closed=state_->input.close(); if(!closed) return abort(pipe_error("close process input",closed.error()).error());
                    writing=false; progress=true;
                } else {
                    auto sent=state_->input.write(bytes.subspan(result.input_written,std::min(buffer.size(),bytes.size()-result.input_written)));
                    if(sent) { result.input_written+=*sent; progress=*sent!=0; }
                    else if(sent.error().code==io::ErrorCode::closed) {
                        result.input_closed_early=true; (void)state_->input.close(); writing=false; progress=true;
                    } else if(sent.error().code!=io::ErrorCode::would_block) return abort(pipe_error("write process input",sent.error()).error());
                }
            }
            auto receive=[&](io::Channel& channel,bool& active,Stream stream,core::Bytes& capture,std::size_t limit,bool& truncated) -> Result<void> {
                if(!active) return {};
                auto read=channel.read(buffer);
                if(!read) {
                    if(read.error().code==io::ErrorCode::would_block) return {};
                    return pipe_error("read process output",read.error());
                }
                if(read->bytes) {
                    progress=true;
                    const auto chunk=core::ByteView(buffer).first(read->bytes);
                    const auto keep=std::min(chunk.size(),limit-capture.size());
                    capture.insert(capture.end(),chunk.begin(),chunk.begin()+static_cast<std::ptrdiff_t>(keep));
                    truncated|=keep<chunk.size();
                    if(options.receive) options.receive(stream,chunk);
                }
                if(read->eof) { active=false; (void)channel.close(); progress=true; }
                return {};
            };
            auto received=receive(state_->output,output,Stream::output,result.output,options.output_limit,result.output_truncated);
            if(!received) return abort(received.error());
            received=receive(state_->error,error,Stream::error,result.error,options.error_limit,result.error_truncated);
            if(!received) return abort(received.error());
            if(completed && !output && !error) return result;
            if(completed && drain.expired()) return abort(core::Error{ErrorCode::failed,"Process exited but an external pipe owner prevented EOF"});
            if(!progress) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    } catch (...) { return abort(core::Error{ErrorCode::failed,"Process communication callback or allocation failed",std::current_exception()}); }
}
} // namespace ii::process
