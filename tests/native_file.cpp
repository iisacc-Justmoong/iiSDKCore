#include "file_fixture.h"
#include <iiSDKCore/IO/NativePipe.h>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>

namespace { std::atomic<bool> fail_allocation = false; int notifications = 0; }
void *operator new(std::size_t size) {
    if (fail_allocation.exchange(false)) throw std::bad_alloc();
    if (auto *value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void operator delete(void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
namespace {
using namespace ii::io;
int checks = 0;
void check(bool valid) { ++checks; if (!valid) throw std::runtime_error("native stream ownership " + std::to_string(checks)); }
void closed(const std::filesystem::path &) noexcept { ++notifications; }
std::intptr_t open_native(const std::filesystem::path &path) {
#if defined(_WIN32)
    return reinterpret_cast<std::intptr_t>(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
#else
    return ::open(path.c_str(), O_RDWR | O_CLOEXEC);
#endif
}
bool retired(std::intptr_t handle) {
#if defined(_WIN32)
    DWORD flags = 0; return !GetHandleInformation(reinterpret_cast<HANDLE>(handle), &flags) && GetLastError() == ERROR_INVALID_HANDLE;
#else
    return fcntl(static_cast<int>(handle), F_GETFD) == -1 && errno == EBADF;
#endif
}
}
int main() {
    using namespace ii::io;
    const auto path = std::filesystem::current_path() / "native-file-ownership.bin";
    try {
        auto provisioned = ii::testing::open_file(path, {Access::read_write, Creation::truncate});
        check(provisioned && bool(provisioned->close()));
        auto handle = open_native(path);
        {
            auto channel = NativeFile::adopt(handle, Access::read_write, path, &closed);
            check(bool(channel));
            {
                auto alias = *channel;
                check(bool(alias.flush()) && notifications == 0);
            }
            check(notifications == 0);
        }
        check(retired(handle) && notifications == 1);
        handle = open_native(path);
        auto non_pipe = NativePipe::adopt(handle, Access::read);
        check(!non_pipe && non_pipe.error().code == ErrorCode::unsupported && retired(handle));
        handle = open_native(path);
        fail_allocation = true;
        bool pipe_failed = false;
        try { (void)NativePipe::adopt(handle, Access::read); } catch (const std::bad_alloc &) { pipe_failed = true; }
        fail_allocation = false;
        check(pipe_failed && retired(handle));
        handle = open_native(path);
        auto explicit_close = NativeFile::adopt(handle, Access::read_write, path, &closed);
        auto alias = *explicit_close;
        check(bool(explicit_close->close()) && bool(alias.close()) && notifications == 2 && retired(handle));
        handle = open_native(path);
        auto invalid = NativeFile::adopt(handle, static_cast<Access>(99), path, &closed);
        check(!invalid && invalid.error().code == ErrorCode::invalid_argument && retired(handle) && notifications == 2);
        handle = open_native(path);
        fail_allocation = true;
        bool failed = false;
        try { (void)NativeFile::adopt(handle, Access::read); } catch (const std::bad_alloc &) { failed = true; }
        fail_allocation = false;
        check(failed && retired(handle) && notifications == 2);
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
        handle = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        auto non_file = NativeFile::adopt(handle, Access::read);
        check(!non_file && non_file.error().code == ErrorCode::unsupported && retired(handle));
#endif
        check(!NativeFile::adopt(-1, Access::read));
        std::filesystem::remove(path);
        std::cout << checks << " native stream ownership checks passed\n";
    } catch (const std::exception &error) { fail_allocation = false; std::cerr << error.what() << '\n'; return 1; }
}
