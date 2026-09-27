#include <iiSDKCore/Runtime/EventLoop.h>
#include <iiSDKCore/Runtime/Stream.h>
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
    auto source_file = ii::io::open_file(__FILE__);
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
    return io_added == 0 ? 0 : 11;
}
