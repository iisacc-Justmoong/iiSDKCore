#include <iiSDKCore/IO/AsyncIO.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#endif

using namespace ii::io;
using namespace std::chrono_literals;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> void fails(const Result<T>& result, ErrorCode error, const char* message) {
    check(!result && result.error().code == error, message);
}
struct TestFile {
    std::filesystem::path path;
    explicit TestFile(const std::filesystem::path& directory) : path(directory /
        ("iisdk-io-" + std::to_string(ii::runtime::Clock::now().time_since_epoch().count()))) {}
    ~TestFile() { std::error_code error; std::filesystem::remove(path, error); }
};
int run(const std::filesystem::path& directory) {
    CancellationToken retained;
    {
        CancellationSource source;
        retained = source.token();
        check(!retained.cancelled() && source.cancel() && !source.cancel(), "cancellation transition");
    }
    check(retained.cancelled() && !CancellationToken{}.cancelled(), "token retains state after source destruction");
    TestFile fixture(directory);
    fails(open_file(fixture.path), ErrorCode::not_found, "missing file");
    auto opened = open_file(fixture.path, {.access=Access::read_write, .creation=Creation::create_new});
    check(bool(opened), "create native file");
    auto channel = *opened;
    check(channel.capabilities().readable && channel.capabilities().writable &&
          channel.capabilities().seekable, "file capabilities");
    fails(open_file(fixture.path, {.access=Access::write, .creation=Creation::create_new}), ErrorCode::already_exists, "exclusive creation");
    std::array<std::byte, 5> source{std::byte{0}, std::byte{1}, std::byte{128}, std::byte{255}, std::byte{42}};
    check(channel.write(source) == source.size(), "binary write");
    check(channel.size() == 5 && bool(channel.flush()), "size and flush");
    check(channel.seek(0) == 0, "seek");
    std::array<std::byte, 8> destination{};
    auto read = channel.read(destination);
    check(read && read->bytes == 5 && !read->eof && std::memcmp(source.data(), destination.data(), 5) == 0, "short read preserves bytes");
    auto eof = channel.read(destination);
    check(eof && eof->bytes == 0 && eof->eof, "EOF distinct from would_block");
    check(channel.read({})->bytes == 0 && !channel.read({})->eof, "empty read not EOF");
    fails(channel.seek(-1), ErrorCode::invalid_argument, "negative absolute seek");
    check(bool(channel.close()), "explicit close");
    fails(opened->read(destination), ErrorCode::closed, "aliases share close state");
    check(bool(channel.close()), "idempotent close");
    auto readonly = open_file(fixture.path).value();
    fails(readonly.write(source), ErrorCode::permission_denied, "read-only channel");
    fails(open_file(fixture.path, {.access=Access::read, .creation=Creation::truncate}), ErrorCode::invalid_argument, "reject invalid options before truncating");
    check(readonly.size() == 5, "invalid open preserved content");
    check(bool(readonly.close()), "close read-only");
    fails(open_file(std::filesystem::path(std::string("invalid\0suffix", 14))), ErrorCode::invalid_argument, "embedded NUL rejected");

    if (ii::runtime::Thread::supported()) {
        ii::runtime::Runtime runtime({.workers=2, .capacity=16});
        auto file = open_file(fixture.path, {.access=Access::read_write}).value();
        AsyncIO io(runtime, file);
        Buffer buffer(8);
        auto alias = buffer;
        check(buffer.bytes().data() == alias.bytes().data(), "buffer storage shared for reuse");
        bool complete = false;
        const auto caller = ii::runtime::Thread::current_id();
        check(bool(io.queries().post(Read{buffer}, [&](ii::runtime::QueryReply<IOResult> reply) {
            check(reply && *reply && (**reply).transfer.bytes == 5 &&
                  ii::runtime::Thread::current_id() != caller, "asynchronous I/O result");
            check(std::memcmp(buffer.bytes().data(), source.data(), 5) == 0, "asynchronous buffer contents");
            complete = true;
        })), "submit read query");
        check(bool(runtime.wait_idle()) && complete, "asynchronous completion");
        CancellationSource stop;
        stop.cancel();
        bool cancelled = false;
        check(bool(io.queries().post(Read{buffer, 0, 5, 1s, stop.token()}, [&](ii::runtime::QueryReply<IOResult> reply) {
            cancelled = reply && !*reply && reply->error().code == ErrorCode::cancelled;
        })), "submit cancelled query");
        check(bool(runtime.wait_idle()) && cancelled, "cooperative cancellation");
        bool invalid = false;
        check(bool(io.queries().post(Read{buffer, 9}, [&](ii::runtime::QueryReply<IOResult> reply) {
            invalid = reply && !*reply && reply->error().code == ErrorCode::invalid_argument;
        })), "submit invalid range");
        check(bool(runtime.wait_idle()) && invalid, "bounds validated before IO");
        int ordered = 0;
        check(bool(io.queries().post(Seek{0}, [&](ii::runtime::QueryReply<IOResult> reply) {
            check(reply && *reply && ordered++ == 0, "FIFO seek");
        })), "submit seek");
        check(bool(io.queries().post(Write{buffer, 1, 3}, [&](ii::runtime::QueryReply<IOResult> reply) {
            check(reply && *reply && (**reply).transfer.bytes == 3 && ordered++ == 1, "FIFO partial-buffer write");
        })), "submit write");
        check(bool(io.queries().post(Flush{}, [&](ii::runtime::QueryReply<IOResult> reply) {
            check(reply && *reply && ordered++ == 2, "FIFO flush");
        })), "submit flush");
        check(bool(runtime.wait_idle()) && ordered == 3, "ordered operations completed");
        check(file.seek(0) == 0, "rewind after async write");
        auto written = file.read(destination);
        check(written && written->bytes == 5 && std::memcmp(destination.data(), source.data()+1, 3) == 0, "async write reached actual storage");
        check(file.seek(0) == 0, "rewind for owning buffer");
        bool owned = false;
        check(bool(io.queries().post(Read{Buffer(8)}, [&](ii::runtime::QueryReply<IOResult> reply) {
            owned = reply && *reply && (**reply).transfer.bytes == 5 && (**reply).buffer.bytes()[0] == source[1];
        })), "temporary buffer retained by request");
        check(bool(runtime.wait_idle()) && owned, "buffer survives caller handle destruction");
        check(bool(io.queries().post(Close{}, [&](ii::runtime::QueryReply<IOResult> reply) {
            check(reply && *reply, "async close");
        })), "submit close");
        check(bool(runtime.wait_idle()), "close completed");
        fails(file.read(destination), ErrorCode::closed, "async close affects shared endpoint");
        check(bool(runtime.join()), "join IO workers");
    }

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)
    struct Master { int fd = -1; ~Master() { if (fd >= 0) ::close(fd); } } master;
    master.fd = posix_openpt(O_RDWR | O_NOCTTY);
    check(master.fd >= 0 && grantpt(master.fd) == 0 && unlockpt(master.fd) == 0, "create isolated PTY fixture");
    auto serial = open_serial(ptsname(master.fd)).value();
    check(serial.capabilities().kind == ChannelKind::serial && !serial.capabilities().seekable, "serial capabilities");
    fails(serial.seek(0), ErrorCode::unsupported, "serial seek unsupported");
    fails(serial.read(destination), ErrorCode::would_block, "serial empty is not EOF");
    fails(serial.wait(Interest::readable, 10ms), ErrorCode::timed_out, "serial wait timeout");
    check(::write(master.fd, source.data(), source.size()) == 5, "feed serial fixture");
    check(bool(serial.wait(Interest::readable, 1s)), "serial readiness");
    auto received = serial.read(destination);
    check(received && received->bytes == 5 && std::memcmp(source.data(), destination.data(), 5) == 0, "serial binary read");
    check(serial.write(source) == 5, "serial binary write");
    check(::read(master.fd, destination.data(), destination.size()) == 5 &&
          std::memcmp(source.data(), destination.data(), 5) == 0, "serial hardware endpoint bytes");
    CancellationSource stopping;
    ii::runtime::Thread waiter;
    Result<void> waiting;
    const auto deadline = ii::runtime::Clock::now() + 2s;
    check(bool(waiter.start([&](ii::runtime::ThreadContext&) {
        // The observer below can own the channel briefly. Retry admission busy;
        // only an admitted wait holds the channel until cancellation/timeout.
        do {
            waiting=serial.wait(Interest::readable, 5s, stopping.token());
            if (waiting || waiting.error().code != ErrorCode::busy) break;
            std::this_thread::yield();
        } while (!stopping.token().cancelled() && ii::runtime::Clock::now() < deadline);
    })), "start readiness worker");
    bool busy = false;
    while (!busy && ii::runtime::Clock::now() < deadline) {
        auto state = serial.size();
        busy = !state && state.error().code == ErrorCode::busy;
        std::this_thread::yield();
    }
    std::optional<Result<void>> close_attempt;
    if (busy) close_attempt=serial.close();
    stopping.cancel();
    check(bool(waiter.join()), "join readiness worker");
    check(busy, "concurrent operation rejected while waiting");
    fails(*close_attempt, ErrorCode::busy, "close cannot invalidate in-flight native handle");
    fails(waiting, ErrorCode::cancelled, "cancel active wait");
    std::cout << "native serial PTY roundtrip passed\n";
#elif defined(__EMSCRIPTEN__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    fails(open_serial("device"), ErrorCode::unsupported, "WASM hardware permissions not emulated");
#if defined(__EMSCRIPTEN__)
    check(channel.capabilities().backend == Backend::wasm_virtual, "WASM storage explicitly virtual");
#endif
#endif
    std::cout << "I/O contracts passed on " << ii::runtime::platform_name() << '\n';
    return 0;
}
int main(int argc, char** argv) {
    try { return run(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::current_path()); }
    catch (const std::exception& error) { std::cerr << "I/O failure: " << error.what() << '\n'; return 1; }
}
