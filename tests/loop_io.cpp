#include <iiSDKCore/Runtime/EventLoop.h>
#include <iiSDKCore/IO/AsyncIO.h>
#include <iiSDKCore/Runtime/Thread.h>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <cstdlib>
#endif

using namespace ii::runtime;
using namespace ii::io;
using namespace std::chrono_literals;
namespace {
constexpr PumpBudget generous{128, 1s};
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture final {
    std::filesystem::path path=std::filesystem::current_path()/
        ("loop-io-"+std::to_string(Clock::now().time_since_epoch().count()));
    ~Fixture() { std::error_code ignored; std::filesystem::remove(path, ignored); }
};
void file_contracts() {
    Fixture fixture;
    auto channel=open_file(fixture.path, {.access=Access::read_write, .creation=Creation::create_new}).value();
    EventLoop loop(2), other(2);
    Buffer buffer(8);
    buffer.bytes()[0]=std::byte{42}; buffer.bytes()[1]=std::byte{255};
    int completions=0;
    auto write=loop.write(channel, Write{buffer,0,2}, [&](IOResult reply) {
        check(reply && reply->transfer.bytes==2, "loop writes real bytes");
        ++completions;
    });
    check(bool(write) && write.token.pending() && completions==0, "loop write is deferred");
    check(!other.cancel(write.token), "cross-loop I/O token rejected");
    check(loop.poll({0,1s})==0 && completions==0, "I/O honors callback budget");
    check(loop.poll(generous)==1 && completions==1 && !write.token.pending(), "one-shot write completion");
    check(channel.seek(0)==0, "rewind loop output");
    Buffer readback(8);
    check(bool(loop.read(channel, Read{readback}, [&](IOResult reply) {
        check(reply && reply->transfer.bytes==2 && readback.bytes()[0]==std::byte{42} &&
              readback.bytes()[1]==std::byte{255}, "loop input preserves binary data");
        ++completions;
    })), "admit loop input");
    check(loop.poll(generous)==1 && completions==2, "loop input emitted");
    check(bool(loop.read(channel, Read{readback}, [&](IOResult reply) {
        check(reply && reply->transfer.eof && reply->transfer.bytes==0, "loop EOF result");
    })) && loop.poll(generous)==1, "loop EOF completes once");
    check(bool(loop.read(channel, Read{readback,0,0,1s}, [&](IOResult reply) {
        check(reply && !reply->transfer.eof && reply->transfer.bytes==0, "empty read does not wait or report EOF");
    })) && loop.poll(generous)==1, "empty read completion");
    check(channel.seek(0)==0, "rewind for query emission");
    QueryPort<int,int> receiver([](const int& value){return value+1;});
    bool queried=false;
    check(bool(loop.read(channel,Read{readback,0,2},[&](IOResult reply) {
        check(bool(reply), "query input read");
        check(bool(receiver.post(loop,std::to_integer<int>(reply->buffer.bytes()[0]),[&](QueryReply<int> result) {
            queried=result && *result==43;
        })), "input completion emits typed query");
    })) && loop.poll(generous)==2 && queried, "I/O and typed object queries share dispatch");
    check(loop.read(channel, Read{readback,9}, [](IOResult){}).error==SubmitError::invalid,
          "loop rejects invalid buffer range before admission");
    check(loop.write(channel, Write{readback,0,1,-1ms}, [](IOResult){}).error==SubmitError::invalid,
          "loop rejects negative timeout");
    check(loop.read(channel,Read{readback,0,1,std::chrono::milliseconds::min()},[](IOResult){}).error==SubmitError::invalid,
          "minimum negative timeout rejected without overflow");
    check(loop.watch(channel,Interest::readable,{}).error==SubmitError::invalid, "empty readiness callback rejected");
    check(loop.read(channel,Read{readback},{}).error==SubmitError::invalid, "empty transfer callback rejected");

    auto watched=loop.watch(channel,Interest::readable,[&](Result<void> ready) {
        check(bool(ready), "file readiness"); ++completions;
    });
    loop.suspend();
    check(loop.poll(generous)==0 && watched.token.pending(), "suspend preserves readiness");
    loop.resume();
    check(loop.poll(generous)==1 && completions==3, "resume readiness");
    check(bool(loop.watch(channel,Interest::readable,[](Result<void>){ throw std::runtime_error("failure"); })),
          "admit throwing I/O callback");
    bool propagated=false;
    try { loop.poll(generous); } catch (const std::runtime_error&) { propagated=true; }
    check(propagated && loop.stats().pending==0, "I/O callback exception frees slot");

    bool destroyed=false;
    struct Capture final { bool& destroyed; ~Capture() { destroyed=true; } };
    auto capture=std::shared_ptr<Capture>(new Capture{destroyed});
    auto pending=loop.watch(channel,Interest::readable,[capture](Result<void>){});
    capture.reset();
    auto second=loop.watch(channel,Interest::writable,[](Result<void>){});
    check(bool(second) && loop.post([]{}).error==SubmitError::full, "I/O shares bounded task storage");
    check(pending.token.cancel() && destroyed && !pending.token.pending(), "cancel clears I/O captures and capacity");
    auto replacement=loop.watch(channel,Interest::readable,[](Result<void>){});
    check(bool(replacement) && !pending.token.cancel(), "I/O generation protects replacement");
    EventLoop fairness(16);
    int background=0;
    for (int i=0;i<10;++i) check(bool(fairness.watch(channel,Interest::readable,[](Result<void>){},
        {.priority=Priority::high})), "admit high-priority I/O");
    check(bool(fairness.watch(channel,Interest::readable,[&](Result<void>){++background;},
        {.priority=Priority::background})), "admit background I/O");
    check(fairness.poll({7,1s})==7 && background==1, "ready I/O obeys priority fairness");
    loop.request_stop();
    check(loop.stats().pending==0 && !second.token.pending() && !replacement.token.pending(), "stop retires watches");
    check(loop.read(channel,Read{buffer},[](IOResult){}).error==SubmitError::stopped, "stop closes I/O admission");
    check(bool(channel.close()), "close file channel");

    EventLoop closed;
    bool reported=false;
    check(bool(closed.watch(channel,Interest::readable,[&](Result<void> ready) {
        reported=!ready && ready.error().code==ErrorCode::closed;
    })) && closed.poll(generous)==1 && reported, "closed watched channel completes with error");
    EventLoop retiring(2);
    bool reentered=false,posted=false;
    struct Retirement final {
        EventLoop& loop; bool& reentered; bool& posted;
        ~Retirement() { reentered=true; posted=bool(loop.post([]{})); }
    };
    auto resource=std::shared_ptr<Retirement>(new Retirement{retiring,reentered,posted});
    auto token=retiring.watch(channel,Interest::readable,[resource](Result<void>){});
    resource.reset();
    check(token.token.cancel() && reentered && posted && retiring.poll(generous)==1,
          "I/O capture retirement can reenter loop outside its mutex");
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
void fifo_contracts() {
    Fixture fixture;
    check(::mkfifo(fixture.path.c_str(),0600)==0, "create FIFO fixture");
    auto channel=open_device(fixture.path,{.access=Access::read_write}).value();
    check(channel.capabilities().kind==ChannelKind::device && !channel.capabilities().seekable,
          "generic nonblocking device capabilities");
    EventLoop loop(4);
    Buffer buffer(8);
    bool expired=false;
    check(bool(loop.read(channel,Read{buffer,0,8,10ms},[&](IOResult reply) {
        expired=!reply && reply.error().code==ErrorCode::timed_out;
    })), "admit idle device read");
    check(loop.poll(generous)==0, "no callback before input or deadline");
    std::this_thread::sleep_for(15ms);
    check(loop.poll(generous)==1 && expired, "I/O timeout completes once");
    bool probe=false;
    check(bool(loop.read(channel,Read{buffer},[&](IOResult reply) {
        probe=!reply && reply.error().code==ErrorCode::would_block;
    })) && loop.poll(generous)==1 && probe, "zero timeout is readiness probe");
    CancellationSource stopping;
    bool cancelled=false;
    check(bool(loop.read(channel,Read{buffer,0,8,1s,stopping.token()},[&](IOResult reply) {
        cancelled=!reply && reply.error().code==ErrorCode::cancelled;
    })), "admit cancellable device read");
    stopping.cancel();
    check(loop.poll(generous)==1 && cancelled, "cooperative I/O cancellation reports result");
    int delivered=0;
    check(bool(loop.watch(channel,Interest::readable,[&](Result<void> ready) {
        check(bool(ready), "FIFO readiness emitted");
        auto read=channel.read(buffer.bytes());
        check(read && read->bytes==2 && buffer.bytes()[0]==std::byte{7}, "FIFO ready input consumed");
        ++delivered;
    })), "watch FIFO");
    check(loop.poll(generous)==0, "idle FIFO watch remains pending");
    std::array<std::byte,2> input{std::byte{7},std::byte{128}};
    check(channel.write(input)==2, "feed FIFO");
    check(loop.poll(generous)==1 && delivered==1 && loop.poll(generous)==0, "FIFO watch fires only once");
    check(bool(channel.close()), "close FIFO");
    Fixture disconnected;
    check(::mkfifo(disconnected.path.c_str(),0600)==0, "create disconnect fixture");
    auto reader=open_device(disconnected.path,{.access=Access::read}).value();
    auto writer=open_device(disconnected.path,{.access=Access::write}).value();
    check(bool(reader.close()), "disconnect FIFO reader");
    auto broken=writer.write(input);
    check(!broken && broken.error().code==ErrorCode::closed, "broken FIFO reports EPIPE without SIGPIPE termination");
    sigset_t blocked{},previous{},pending_signals{};
    sigemptyset(&blocked); sigaddset(&blocked,SIGPIPE);
    check(pthread_sigmask(SIG_BLOCK,&blocked,&previous)==0, "block fixture signal");
    const bool raised=raise(SIGPIPE)==0;
    broken=writer.write(input);
    const bool inspected=sigpending(&pending_signals)==0;
    const bool preserved=inspected && sigismember(&pending_signals,SIGPIPE)==1;
    if (preserved) { int signal=0; (void)sigwait(&blocked,&signal); }
    const bool restored=pthread_sigmask(SIG_SETMASK,&previous,nullptr)==0;
    check(raised && preserved && restored && !broken, "device write preserves host pending SIGPIPE and signal mask");
    Fixture regular;
    auto file=open_file(regular.path,{.access=Access::write,.creation=Creation::create_new}).value();
    auto invalid=open_device(regular.path);
    check(!invalid && invalid.error().code==ErrorCode::unsupported, "device open does not disguise regular files");

    Fixture saturated;
    check(::mkfifo(saturated.path.c_str(),0600)==0, "create backpressure FIFO");
    auto full=open_device(saturated.path,{.access=Access::read_write}).value();
    std::array<std::byte,1024> fill{};
    std::size_t filled=0;
    for (;;) {
        auto written=full.write(fill);
        if (!written) { check(written.error().code==ErrorCode::would_block, "FIFO saturation is nonblocking"); break; }
        filled+=*written;
        check(filled<4*1024*1024, "bounded FIFO fixture capacity");
    }
    EventLoop pressure(4);
    Buffer outgoing(2);
    outgoing.bytes()[0]=std::byte{3}; outgoing.bytes()[1]=std::byte{4};
    bool emitted=false,unblocked=false;
    check(bool(pressure.write(full,Write{outgoing,0,2,1s},[&](IOResult reply) {
        check(reply && reply->transfer.bytes==2, "backpressure output resumes after readiness");
        emitted=true; pressure.request_stop();
    })), "admit blocked output");
    check(pressure.poll(generous)==0, "full output does not block host poll");
    Buffer drained(filled);
    check(bool(pressure.after(10ms,[&] {
        auto result=full.read(drained.bytes());
        check(result && result->bytes>0, "timer consumes pending input");
        unblocked=true;
    })), "timer progresses beside blocked output");
    check(bool(pressure.after(2s,[&]{pressure.request_stop();})), "backpressure fixture watchdog");
    pressure.run(generous);
    check(emitted && unblocked && pressure.stats().pending==0, "I/O backpressure and timers share native wait");
}
#endif

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
struct SerialFixture final {
    int master=-1;
    Channel channel;
    SerialFixture() {
        master=posix_openpt(O_RDWR|O_NOCTTY|O_NONBLOCK);
        check(master>=0 && grantpt(master)==0 && unlockpt(master)==0, "create PTY");
        channel=open_serial(ptsname(master)).value();
    }
    ~SerialFixture() { if (master>=0) ::close(master); }
};
void serial_contracts() {
    SerialFixture first, second;
    EventLoop loop(8);
    Buffer input(8), output(8);
    output.bytes()[0]=std::byte{99}; output.bytes()[1]=std::byte{255};
    bool read=false, write=false, task=false, timeout=false;
    check(bool(loop.read(first.channel,Read{input,0,8,2s},[&](IOResult reply) {
        check(reply && reply->transfer.bytes==2 && input.bytes()[0]==std::byte{42}, "reactor reads PTY input");
        read=true;
        check(bool(loop.write(second.channel,Write{output,0,2,1s},[&](IOResult sent) {
            check(sent && sent->transfer.bytes==2, "reactor emits PTY output");
            write=true;
        })), "submit output from input completion");
    })), "admit serial input");
    check(bool(loop.read(second.channel,Read{Buffer(4),0,4,40ms},[&](IOResult reply) {
        check(!reply && reply.error().code==ErrorCode::timed_out, "unrelated idle channel timeout");
        timeout=true;
    })), "admit independent channel");
    check(bool(loop.post([&]{task=true;})), "admit CPU task beside idle I/O");
    check(bool(loop.after(100ms,[&]{loop.request_stop();})), "schedule driver stop");
    std::array<std::byte,2> bytes{std::byte{42},std::byte{128}};
    check(::write(first.master,bytes.data(),bytes.size())==2, "feed PTY input");
    loop.run(generous);
    check(read && write && task && timeout && loop.stats().pending==0, "hardware and timers share loop");
    std::array<std::byte,8> received{};
    check(::read(second.master,received.data(),received.size())==2 && received[0]==std::byte{99} &&
          received[1]==std::byte{255}, "emitted output reaches peer");

    if (Thread::supported()) {
        EventLoop waiting(2);
        std::atomic<bool> invoked=false;
        auto pending=waiting.watch(first.channel,Interest::readable,[&](Result<void>){invoked=true;});
        check(bool(pending), "admit native indefinite watch");
        Thread runner;
        check(bool(runner.start(waiting)), "start native I/O driver");
        const auto deadline=Clock::now()+1s;
        while (waiting.stats().io_waits==0 && Clock::now()<deadline) std::this_thread::yield();
        check(waiting.stats().io_waits>0, "driver entered native I/O wait");
        std::atomic<bool> awake=false;
        check(bool(waiting.post([&]{awake=true;})), "post while kernel watch is active");
        while (!awake && Clock::now()<deadline) std::this_thread::yield();
        check(awake && pending.token.pending(), "posted task wakes driver without waiting for hardware");
        check(pending.token.cancel() && !pending.token.pending(), "cancel active kernel watch");
        check(bool(first.channel.close()), "cancel has released every native wait lease");
        std::atomic<bool> posted=false;
        check(bool(waiting.post([&]{posted=true; waiting.request_stop();})), "post wakes idle native driver");
        check(bool(runner.join()) && posted && !invoked && waiting.stats().pending==0, "native wakeup and watch cancellation");
    }
}
#endif
}
int main() {
    try {
        file_contracts();
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
        fifo_contracts();
#else
        check(!open_device("device") && open_device("device").error().code==ErrorCode::unsupported,
              "unavailable generic device backend reported");
#endif
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
        serial_contracts();
#endif
        std::cout << "event loop I/O contracts passed on " << platform_name() << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "loop I/O failure: " << error.what() << '\n'; return 1; }
}
