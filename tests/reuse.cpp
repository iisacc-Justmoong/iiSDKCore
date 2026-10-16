#include "file_fixture.h"
#include <iiSDKCore/Runtime/EventLoop.h>
#include <iiSDKCore/Runtime/FrameLoop.h>
#include <iiSDKCore/Runtime/Stream.h>
#include <iiSDKCore/Runtime/LiveStream.h>
#include <iiSDKCore/Runtime/Runtime.h>
#include <iiSDKCore/IO/AsyncIO.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>

namespace { std::atomic<std::size_t> allocations{0}; }

void* operator new(std::size_t size) {
    ++allocations;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    using namespace ii::runtime;
    EventLoop loop(8);
    Stream<int> stream(4);
    int received = 0;
    // Warm up library/platform lazy initialization before measuring the hot path.
    (void)loop.post([] {});
    loop.poll();
    const auto before = allocations.load();
    for (int i = 0; i < 10000; ++i) {
        if (!loop.post([] {})) return 1;
        if (loop.poll({1, std::chrono::seconds(1)}) != 1) return 2;
        if (!stream.try_push(i) || !stream.try_pop(received) || received != i) return 3;
    }
    const auto added = allocations.load() - before;
    std::cout << "reuse: 10000 iterations, operator new calls=" << added << '\n';
    if (added != 0) return 4;
    Runtime runtime({.workers = 2, .capacity = 8});
    SerialQueue object(runtime);
    if (!object.post([] {}) || !runtime.wait_idle()) return 5;
    const auto parallel_before = allocations.load();
    for (int i = 0; i < 10000; ++i) {
        if (!object.post([] {}) || !runtime.wait_idle()) return 6;
    }
    const auto parallel_added = allocations.load() - parallel_before;
    std::cout << "parallel reuse: 10000 iterations, operator new calls=" << parallel_added << '\n';
    if (parallel_added != 0 || !runtime.join()) return 7;
    auto source_file = ii::testing::open_file(__FILE__);
    if (!source_file) return 8;
    ii::io::Buffer buffer(64);
    auto warm = source_file->read(buffer.bytes());
    if (!warm || !source_file->seek(0)) return 9;
    const auto io_before = allocations.load();
    for (int i=0; i<10000; ++i) {
        auto bytes = source_file->read(buffer.bytes());
        if (!bytes || bytes->bytes != 64 || !source_file->seek(0)) return 10;
    }
    const auto io_added = allocations.load() - io_before;
    std::cout << "I/O buffer reuse: 10000 iterations, operator new calls=" << io_added << '\n';
    if (io_added!=0) return 11;
    int io_completed=0;
    auto completion=[&io_completed](ii::io::IOResult reply) {
        if (reply && reply->transfer.bytes==64) ++io_completed;
        else io_completed=-1000000;
    };
    if (!source_file->seek(0) || !loop.read(*source_file,ii::io::Read{buffer},completion) ||
        loop.poll({1,std::chrono::seconds(1)})!=1 || !source_file->seek(0)) return 12;
    const auto loop_io_before=allocations.load();
    for (int i=0;i<10000;++i) {
        if (!loop.read(*source_file,ii::io::Read{buffer},completion) || loop.poll({1,std::chrono::seconds(1)})!=1 ||
            !source_file->seek(0)) return 13;
    }
    const auto loop_io_added=allocations.load()-loop_io_before;
    std::cout << "event loop I/O reuse: 10000 iterations, operator new calls=" << loop_io_added << '\n';
    if (loop_io_added!=0 || io_completed!=10001) return 14;
    LiveStream<int> live(4,std::chrono::seconds(1));
    RealtimeSchedule schedule{std::chrono::seconds(1),std::chrono::seconds(1),
        std::chrono::milliseconds(10),std::chrono::milliseconds(10),{}};
    const auto timed_before=allocations.load();
    for (int i=0;i<10000;++i) {
        if (!loop.realtime(schedule,[](const auto&) { return SliceResult::stop; }) ||
            loop.poll({1,std::chrono::seconds(1)})!=1 || !live.push(Clock::now(),i)) return 15;
        auto frame=live.take();
        if (!frame || frame->value!=i) return 16;
    }
    const auto timed_added=allocations.load()-timed_before;
    std::cout << "timed/live reuse: 10000 iterations, operator new calls=" << timed_added << '\n';
    if (timed_added!=0 || !live.close()) return 17;
    FrameLoop frames(loop,{.capacity=4,.fixed_step=std::chrono::milliseconds(1)});
    int frame_calls=0;
    if (!frames.add([&](const auto&) { ++frame_calls; }) ||
        !frames.add([](const auto&) {},{.phase=FramePhase::fixed_update})) return 18;
    const auto epoch=Clock::now();
    frames.step(epoch); // Warm up the graph's cached schedule.
    const auto frame_before=allocations.load();
    for (int i=1;i<=10000;++i) frames.step(epoch+std::chrono::milliseconds(i));
    const auto frame_added=allocations.load()-frame_before;
    std::cout << "frame reuse: 10000 frames, operator new calls=" << frame_added << '\n';
    return frame_added==0 && frame_calls==10001 ? 0 : 19;
}
