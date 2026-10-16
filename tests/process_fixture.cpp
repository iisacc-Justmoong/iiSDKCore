#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <iiSDKCore/Process/Process.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif
namespace {
int run(std::vector<std::string> args) {
    if(args.size()<2) return 99;
    const auto& mode=args[1];
    if(mode=="closed-stdio") {
#if defined(_WIN32)
        for(auto which:{STD_INPUT_HANDLE,STD_OUTPUT_HANDLE,STD_ERROR_HANDLE}) {
            CloseHandle(GetStdHandle(which)); SetStdHandle(which,INVALID_HANDLE_VALUE);
        }
#else
        for(int fd=0;fd<3;++fd) ::close(fd);
#endif
        ii::process::Launch launch; launch.arguments={args.front(),"echo","safe descriptors"};
        auto child=ii::process::Process::start(launch); if(!child) return 91;
        auto result=child->communicate(); if(!result) return 92;
        const auto& bytes=result->output;
        return result->exit.code==0 && std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size())=="safe descriptors"?0:90;
    }
    if(mode=="echo") { for(std::size_t i=2;i<args.size();++i) std::cout<<args[i]; return 0; }
    if(mode=="delayed-output") {
        std::cout<<'R'<<std::flush;
        if (std::cin.get()!='g') return 97;
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        std::cout<<"42"<<std::flush; return 0;
    }
    if(mode=="args") { for(std::size_t i=2;i<args.size();++i) std::cout<<args[i].size()<<":"<<args[i]<<"\n"; return 0; }
    if(mode=="environment") {
        for(std::size_t i=2;i<args.size();++i) {
#if defined(_WIN32)
            const auto& key=args[i];
            const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,key.data(),static_cast<int>(key.size()),nullptr,0);
            std::wstring name(count,L'\0');
            MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,key.data(),static_cast<int>(key.size()),name.data(),count);
            SetLastError(ERROR_SUCCESS);
            const auto size=GetEnvironmentVariableW(name.c_str(),nullptr,0);
            if(size==0) std::cout<<(GetLastError()==ERROR_ENVVAR_NOT_FOUND?"<unset>":"")<<"\n";
            else {
                std::wstring wide(size,L'\0'); GetEnvironmentVariableW(name.c_str(),wide.data(),size); wide.pop_back();
                const auto bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide.data(),static_cast<int>(wide.size()),nullptr,0,nullptr,nullptr);
                std::string value(bytes,'\0'); WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide.data(),static_cast<int>(wide.size()),value.data(),bytes,nullptr,nullptr);
                std::cout<<value<<"\n";
            }
#else
            const char* value=std::getenv(args[i].c_str()); std::cout<<(value?value:"<unset>")<<"\n";
#endif
        }
        const auto cwd=std::filesystem::current_path().u8string();
        std::cout.write(reinterpret_cast<const char*>(cwd.data()),static_cast<std::streamsize>(cwd.size()));
        return 0;
    }
    if(mode=="exit") return std::stoi(args.at(2));
    if(mode=="sleep" || mode=="leaf") { std::this_thread::sleep_for(std::chrono::seconds(60)); return 0; }
    if(mode=="streams") { std::cout<<"out\n"<<std::flush; std::cerr<<"err\n"<<std::flush; return 7; }
    if(mode=="early-input") {
#if defined(_WIN32)
        _close(0);
#else
        ::close(0);
#endif
        std::cout<<"ready"<<std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(30)); return 0;
    }
    if(mode=="duplex" || mode=="copy") {
        std::array<char,4096> block{};
        if(mode=="duplex") {
            for(int i=0;i<256;++i) {
                for(std::size_t j=0;j<block.size();++j) block[j]=static_cast<char>(j%251);
                std::cout.write(block.data(),block.size()); std::cout.flush();
                std::cerr.write(block.data(),block.size()); std::cerr.flush();
            }
        }
        while(std::cin) { std::cin.read(block.data(),block.size()); std::cout.write(block.data(),std::cin.gcount()); std::cout.flush(); }
        return 0;
    }
#if !defined(__EMSCRIPTEN__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
    if(mode=="tree" || mode=="tree-exit") {
#if defined(_WIN32)
        const auto& name=args.front();
        const auto wide=std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(name.data()),name.size())).native();
        auto command=L"\""+wide+L"\" leaf";
        STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES;
        startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE); startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE); startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION info{};
        if(!CreateProcessW(wide.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&info)) return 95;
        const auto id=info.dwProcessId; CloseHandle(info.hThread); CloseHandle(info.hProcess);
#else
        const auto id=fork();
        if(id<0) return 95;
        if(id==0) { execl(args.front().c_str(),args.front().c_str(),"leaf",static_cast<char*>(nullptr)); _exit(96); }
#endif
        std::cout<<id<<"\n"<<std::flush;
        if(mode=="tree") std::this_thread::sleep_for(std::chrono::seconds(60));
        return 0;
    }
    if(mode=="inherited-handle") {
#if defined(_WIN32)
        DWORD flags=0; return GetHandleInformation(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(args.at(2)))),&flags)?94:0;
#else
        return fcntl(std::stoi(args.at(2)),F_GETFD)<0 && errno==EBADF?0:94;
#endif
    }
    if(mode=="signal") {
#if defined(_WIN32)
        return 93;
#else
        raise(SIGTERM); return 93;
#endif
    }
#endif
    return 98;
}
}
#if defined(_WIN32)
int wmain(int argc,wchar_t** argv) {
    _setmode(0,_O_BINARY); _setmode(1,_O_BINARY); _setmode(2,_O_BINARY);
    std::vector<std::string> args;
    for(int i=0;i<argc;++i) {
        auto size=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);
        std::string text(size,'\0'); WideCharToMultiByte(CP_UTF8,0,argv[i],-1,text.data(),size,nullptr,nullptr); text.pop_back(); args.push_back(std::move(text));
    }
    return run(std::move(args));
}
#else
int main(int argc,char** argv) { return run({argv,argv+argc}); }
#endif
