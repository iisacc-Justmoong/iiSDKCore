#include "../src/IO/Poller.h"
#include "../src/IO/NativePipe.h"
#include "../src/IO/ReadinessPolicy.h"
#include <array>
#include <chrono>
#include <iostream>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace ii::io;
using namespace std::chrono_literals;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void many_readiness_watches(){
    constexpr std::size_t count=64;
    std::vector<Channel> readers,writers;
    std::vector<detail::WatchEntry> watches;
    readers.reserve(count);writers.reserve(count);watches.reserve(count*2);
    for(std::size_t i=0;i<count;++i){
        int handles[2];check(::pipe(handles)==0,"create native readiness pipe");
        auto reader=NativePipe::adopt(handles[0],Access::read);
        auto writer=NativePipe::adopt(handles[1],Access::write);
        check(bool(reader)&&bool(writer),"adopt readiness endpoints");
        readers.push_back(*reader);writers.push_back(*writer);
    }
    for(std::size_t i=0;i<count;++i)watches.push_back({readers[i],Interest::readable});
    for(std::size_t i=count;i>0;--i)watches.push_back({readers[i-1],Interest::readable});
    detail::ReadinessPoller poller(watches.size());
    poller.prepare(watches,1s);
    std::array<std::byte,1> byte{std::byte{42}};
    auto locked=readers[0].read(byte);
    check(!locked&&locked.error().code==ErrorCode::busy,"prepared set owns channel leases");
    check(writers[count/2].write(byte)==1,"wake a channel in the readiness set");
    poller.wait();
    std::array<std::byte,1> received{};
    check(bool(readers[count/2].wait_polling(Interest::readable,1s,5ms)),"explicit polling request admits ready input");
    const auto result=readers[count/2].read(received);
    check(result&&result->bytes==1&&received==byte,"ready data survives duplicate watches");
    for(auto& channel:readers)check(bool(channel.close()),"release watched reader");
    for(auto& channel:writers)check(bool(channel.close()),"release readiness writer");
}
void contention_waits_for_release(){
    int handles[2]; check(::pipe(handles)==0,"create contention pipe");
    auto reader=NativePipe::adopt(handles[0],Access::read).value();
    auto writer=NativePipe::adopt(handles[1],Access::write).value();
    const auto invalid=reader.wait_polling(Interest::readable,1s,0ms);
    check(!invalid && invalid.error().code==ErrorCode::invalid_argument,"polling interval must be positive");
    CancellationSource cancel;
    auto holding=std::async(std::launch::async,[&] {
        for (;;) {
            auto result=reader.wait(Interest::readable,5s,cancel.token());
            if(result || result.error().code!=ErrorCode::busy) return result;
            std::this_thread::yield();
        }
    });
    std::array<std::byte,1> byte{};
    const auto deadline=std::chrono::steady_clock::now()+1s;
    bool busy=false;
    while(std::chrono::steady_clock::now()<deadline) {
        auto result=reader.read(byte);
        if(!result && result.error().code==ErrorCode::busy) { busy=true; break; }
        std::this_thread::yield();
    }
    detail::ReadinessPoller poller(1);
    detail::ReadinessPoller second(1);
    const std::array watches{detail::WatchEntry{reader,Interest::readable}};
    poller.prepare(watches,3s);
    second.prepare(watches,3s);
    // Destroying a subscribed poller must not leave an owning or dangling
    // callback when the channel eventually becomes available.
    { detail::ReadinessPoller abandoned(1); abandoned.prepare(watches,3s); }
    auto waiting=std::async(std::launch::async,[&] { poller.wait(); });
    auto waiting_second=std::async(std::launch::async,[&] { second.wait(); });
    const bool early=waiting.wait_for(80ms)==std::future_status::ready ||
                     waiting_second.wait_for(0ms)==std::future_status::ready;
    cancel.cancel();
    const auto held=holding.get();
    const bool notified=waiting.wait_for(1s)==std::future_status::ready;
    const bool second_notified=waiting_second.wait_for(1s)==std::future_status::ready;
    if(!notified) poller.wake_persistent();
    if(!second_notified) second.wake_persistent();
    waiting.get();
    waiting_second.get();
    check(busy,"another operation held the watched channel");
    check(!early,"channel contention caused periodic wakeups");
    check(!held && held.error().code==ErrorCode::cancelled && notified && second_notified,
          "channel release did not notify all readiness waits");
}
void platform_polling_policy(){
    Capabilities windows_pipe{ChannelKind::pipe,Backend::windows,true,false,false,false};
    Capabilities windows_file{ChannelKind::file,Backend::windows,true,false,false,false};
    Capabilities posix_pipe{ChannelKind::pipe,Backend::posix,true,false,false,false};
    check(detail::needs_explicit_polling(windows_pipe) && !detail::needs_explicit_polling(windows_file) &&
          !detail::needs_explicit_polling(posix_pipe),"non-overlapped Windows handles require an opt-in");
    check(detail::valid_polling_interval(std::nullopt) && detail::valid_polling_interval(5ms) &&
          !detail::valid_polling_interval(0ms) && !detail::valid_polling_interval(-1ms),
          "only explicit positive polling intervals are valid");
}
}
int main(){
    try{platform_polling_policy();many_readiness_watches();contention_waits_for_release();std::cout<<"readiness set contracts passed\n";}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
