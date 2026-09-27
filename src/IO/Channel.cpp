#include "Channel.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <mutex>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if !defined(__EMSCRIPTEN__)
#include <termios.h>
#endif
#endif

namespace ii::io {
namespace {
auto failure(ErrorCode code, std::error_code native = {}) { return std::unexpected(Error{code, native}); }
auto native_failure() {
#if defined(_WIN32)
    const auto number = GetLastError();
    auto code = ErrorCode::system_error;
    switch (number) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: code=ErrorCode::not_found; break;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: code=ErrorCode::permission_denied; break;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: code=ErrorCode::already_exists; break;
    case ERROR_INVALID_PARAMETER: case ERROR_NEGATIVE_SEEK: code=ErrorCode::invalid_argument; break;
    case ERROR_INVALID_HANDLE: code=ErrorCode::closed; break;
    case ERROR_SEM_TIMEOUT: case ERROR_TIMEOUT: code=ErrorCode::timed_out; break;
    case ERROR_OPERATION_ABORTED: code=ErrorCode::cancelled; break;
    }
    return failure(code, {static_cast<int>(number), std::system_category()});
#else
    const auto number = errno;
    auto code = ErrorCode::system_error;
    switch (number) {
    case ENOENT: case ENOTDIR: code=ErrorCode::not_found; break;
    case EACCES: case EPERM: code=ErrorCode::permission_denied; break;
    case EEXIST: code=ErrorCode::already_exists; break;
    case EINVAL: case EOVERFLOW: code=ErrorCode::invalid_argument; break;
    case EBADF: code=ErrorCode::closed; break;
    case EAGAIN: code=ErrorCode::would_block; break;
    case ENOTSUP: case ENOSYS: case ESPIPE: case ENOTTY: code=ErrorCode::unsupported; break;
    }
    return failure(code, {number, std::generic_category()});
#endif
}
bool valid_path(const std::filesystem::path& path) {
    return !path.empty() && path.native().find(std::filesystem::path::value_type{}) == std::filesystem::path::string_type::npos;
}
bool valid_baud(unsigned baud) {
    return baud == 9600 || baud == 19200 || baud == 38400 || baud == 57600 || baud == 115200;
}
}

PlatformCapabilities platform_capabilities() noexcept {
#if defined(__EMSCRIPTEN__)
    return {Backend::wasm_virtual, true, false};
#elif defined(_WIN32)
    return {Backend::windows, true, true};
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return {Backend::posix, true, false};
#else
    return {Backend::posix, true, true};
#endif
}

struct Channel::State {
    mutable std::mutex mutex;
    Capabilities caps;
#if defined(_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool valid() const { return handle != INVALID_HANDLE_VALUE; }
    int release() { auto old=handle; handle=INVALID_HANDLE_VALUE; return CloseHandle(old) ? 0 : -1; }
#else
    int handle = -1;
    bool valid() const { return handle >= 0; }
    int release() { auto old=handle; handle=-1; return ::close(old); } // Never retry close after EINTR.
#endif
    ~State() { if (valid()) (void)release(); }
    Result<std::unique_lock<std::mutex>> lock() {
        std::unique_lock held(mutex, std::try_to_lock);
        if (!held) return failure(ErrorCode::busy);
        if (!valid()) return failure(ErrorCode::closed);
        return held;
    }
};

Result<Channel> open_file(const std::filesystem::path& path, FileOptions options) {
    if (!valid_path(path) || (options.access != Access::read && options.access != Access::write && options.access != Access::read_write) ||
        (options.creation != Creation::existing && options.creation != Creation::open_or_create &&
         options.creation != Creation::create_new && options.creation != Creation::truncate) ||
        (options.access == Access::read && options.creation != Creation::existing)) return failure(ErrorCode::invalid_argument);
    auto state = std::make_shared<Channel::State>();
    state->caps = {ChannelKind::file, platform_capabilities().backend,
        options.access != Access::write, options.access != Access::read, true, options.access != Access::read};
#if defined(_WIN32)
    DWORD mode = OPEN_EXISTING;
    switch (options.creation) {
    case Creation::open_or_create: mode=OPEN_ALWAYS; break;
    case Creation::create_new: mode=CREATE_NEW; break;
    case Creation::truncate: mode=OPEN_ALWAYS; break;
    default: break;
    }
    const DWORD access = (state->caps.readable ? GENERIC_READ : 0) | (state->caps.writable ? GENERIC_WRITE : 0);
    state->handle=CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, mode, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!state->valid()) return native_failure();
    if (GetFileType(state->handle) != FILE_TYPE_DISK) return failure(ErrorCode::unsupported);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(state->handle, &info)) return native_failure();
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return failure(ErrorCode::unsupported);
    if (options.creation == Creation::truncate && !SetEndOfFile(state->handle)) return native_failure();
#else
    int flags = options.access == Access::read ? O_RDONLY : options.access == Access::write ? O_WRONLY : O_RDWR;
    if (options.creation != Creation::existing) flags |= O_CREAT;
    if (options.creation == Creation::create_new) flags |= O_EXCL;
    do { state->handle = ::open(path.c_str(), flags | O_CLOEXEC | O_NONBLOCK, 0600); } while (!state->valid() && errno == EINTR);
    if (!state->valid()) return native_failure();
    struct stat info{};
    if (fstat(state->handle, &info) != 0) return native_failure();
    if (!S_ISREG(info.st_mode)) return failure(ErrorCode::unsupported);
    if (options.creation == Creation::truncate && ftruncate(state->handle, 0) != 0) return native_failure();
#endif
    return Channel(std::move(state));
}

Result<Channel> open_serial(const std::filesystem::path& path, SerialOptions options) {
    if (!platform_capabilities().serial) return failure(ErrorCode::unsupported);
    if (!valid_path(path) || !valid_baud(options.baud_rate)) return failure(ErrorCode::invalid_argument);
    auto state = std::make_shared<Channel::State>();
    state->caps = {ChannelKind::serial, platform_capabilities().backend, true, true, false, false};
#if defined(_WIN32)
    state->handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (!state->valid()) return native_failure();
    DCB config{};
    config.DCBlength = sizeof(config);
    if (!GetCommState(state->handle, &config)) return native_failure();
    config.BaudRate=options.baud_rate; config.ByteSize=8; config.Parity=NOPARITY; config.StopBits=ONESTOPBIT;
    config.fBinary=TRUE; config.fParity=FALSE; config.fOutxCtsFlow=FALSE; config.fOutxDsrFlow=FALSE;
    config.fDtrControl=DTR_CONTROL_DISABLE; config.fDsrSensitivity=FALSE; config.fTXContinueOnXoff=TRUE;
    config.fOutX=FALSE; config.fInX=FALSE; config.fErrorChar=FALSE; config.fNull=FALSE;
    config.fRtsControl=RTS_CONTROL_DISABLE; config.fAbortOnError=FALSE;
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout=MAXDWORD;
    timeouts.WriteTotalTimeoutConstant=50;
    if (!SetCommState(state->handle, &config) || !SetCommTimeouts(state->handle, &timeouts)) return native_failure();
#elif !defined(__EMSCRIPTEN__)
    do { state->handle=::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC); } while (!state->valid() && errno == EINTR);
    if (!state->valid()) return native_failure();
    termios config{};
    if (tcgetattr(state->handle, &config) != 0) return native_failure();
    cfmakeraw(&config);
    config.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
#ifdef CRTSCTS
    config.c_cflag &= ~CRTSCTS;
#endif
    config.c_cflag |= CS8 | CLOCAL | CREAD;
    config.c_cc[VMIN]=1; config.c_cc[VTIME]=0;
    speed_t speed = B115200;
    switch (options.baud_rate) {
    case 9600: speed=B9600; break; case 19200: speed=B19200; break;
    case 38400: speed=B38400; break; case 57600: speed=B57600; break; default: break;
    }
    if (cfsetispeed(&config, speed) != 0 || cfsetospeed(&config, speed) != 0 || tcsetattr(state->handle, TCSANOW, &config) != 0)
        return native_failure();
#endif
    return Channel(std::move(state));
}

Capabilities Channel::capabilities() const noexcept { return state_ ? state_->caps : Capabilities{}; }
Result<Transfer> Channel::read(std::span<std::byte> destination) const {
    if (!state_) return failure(ErrorCode::closed);
    auto held = state_->lock(); if (!held) return std::unexpected(held.error());
    if (!state_->caps.readable) return failure(ErrorCode::permission_denied);
    if (destination.empty()) return Transfer{};
#if defined(_WIN32)
    DWORD count=0;
    if (!ReadFile(state_->handle, destination.data(), static_cast<DWORD>(std::min<std::size_t>(destination.size(), MAXDWORD)), &count, nullptr)) return native_failure();
    if (!count && state_->caps.kind == ChannelKind::serial) return failure(ErrorCode::would_block);
#else
    ssize_t count;
    do { count=::read(state_->handle, destination.data(), std::min(destination.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()))); } while (count < 0 && errno == EINTR);
    if (count < 0) return native_failure();
#endif
    return Transfer{static_cast<std::size_t>(count), count == 0};
}
Result<std::size_t> Channel::write(std::span<const std::byte> source) const {
    if (!state_) return failure(ErrorCode::closed);
    auto held=state_->lock(); if (!held) return std::unexpected(held.error());
    if (!state_->caps.writable) return failure(ErrorCode::permission_denied);
    if (source.empty()) return 0;
#if defined(_WIN32)
    DWORD count=0;
    if (!WriteFile(state_->handle, source.data(), static_cast<DWORD>(std::min<std::size_t>(source.size(), MAXDWORD)), &count, nullptr)) return native_failure();
#else
    ssize_t count;
    do { count=::write(state_->handle, source.data(), std::min(source.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()))); } while (count < 0 && errno == EINTR);
    if (count < 0) return native_failure();
#endif
    return static_cast<std::size_t>(count);
}
Result<std::uint64_t> Channel::seek(std::int64_t offset, SeekOrigin origin) const {
    if (!state_) return failure(ErrorCode::closed);
    auto held=state_->lock(); if (!held) return std::unexpected(held.error());
    if (!state_->caps.seekable) return failure(ErrorCode::unsupported);
    if ((origin != SeekOrigin::begin && origin != SeekOrigin::current && origin != SeekOrigin::end) ||
        (origin == SeekOrigin::begin && offset < 0)) return failure(ErrorCode::invalid_argument);
#if defined(_WIN32)
    LARGE_INTEGER distance{}, position{}; distance.QuadPart=offset;
    const auto mode = origin == SeekOrigin::begin ? FILE_BEGIN : origin == SeekOrigin::current ? FILE_CURRENT : FILE_END;
    if (!SetFilePointerEx(state_->handle, distance, &position, mode)) return native_failure();
    return static_cast<std::uint64_t>(position.QuadPart);
#else
    if (offset < std::numeric_limits<off_t>::min() || offset > std::numeric_limits<off_t>::max()) return failure(ErrorCode::invalid_argument);
    const auto mode = origin == SeekOrigin::begin ? SEEK_SET : origin == SeekOrigin::current ? SEEK_CUR : SEEK_END;
    const auto position=lseek(state_->handle, static_cast<off_t>(offset), mode);
    if (position < 0) return native_failure();
    return static_cast<std::uint64_t>(position);
#endif
}
Result<std::uint64_t> Channel::size() const {
    if (!state_) return failure(ErrorCode::closed);
    auto held=state_->lock(); if (!held) return std::unexpected(held.error());
    if (!state_->caps.seekable) return failure(ErrorCode::unsupported);
#if defined(_WIN32)
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(state_->handle, &length)) return native_failure();
    return static_cast<std::uint64_t>(length.QuadPart);
#else
    struct stat info{};
    if (fstat(state_->handle, &info) != 0) return native_failure();
    return static_cast<std::uint64_t>(info.st_size);
#endif
}
Result<void> Channel::flush() const {
    if (!state_) return failure(ErrorCode::closed);
    auto held=state_->lock(); if (!held) return std::unexpected(held.error());
    if (!state_->caps.flushable) return failure(ErrorCode::unsupported);
#if defined(_WIN32)
    if (!FlushFileBuffers(state_->handle)) return native_failure();
#else
    int result;
    do { result=fsync(state_->handle); } while (result < 0 && errno == EINTR);
    if (result < 0) return native_failure();
#endif
    return {};
}
Result<void> Channel::close() const {
    if (!state_) return {};
    std::unique_lock held(state_->mutex, std::try_to_lock);
    if (!held) return failure(ErrorCode::busy);
    if (!state_->valid()) return {};
    if (state_->release() != 0) return native_failure();
    return {};
}
Result<void> Channel::wait(Interest interest, std::chrono::milliseconds timeout, CancellationToken stop) const {
    if (!state_) return failure(ErrorCode::closed);
    if (timeout.count() < 0 || (interest != Interest::readable && interest != Interest::writable)) return failure(ErrorCode::invalid_argument);
    auto held=state_->lock(); if (!held) return std::unexpected(held.error());
    if (stop.cancelled()) return failure(ErrorCode::cancelled);
    if ((interest == Interest::readable && !state_->caps.readable) || (interest == Interest::writable && !state_->caps.writable)) return failure(ErrorCode::permission_denied);
    if (state_->caps.kind == ChannelKind::file) return {};
    const auto start=std::chrono::steady_clock::now();
    for (;;) {
        if (stop.cancelled()) return failure(ErrorCode::cancelled);
        const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start);
        const auto remaining=elapsed >= timeout ? std::chrono::milliseconds(0) : timeout-elapsed;
        const auto slice=std::min(remaining, std::chrono::milliseconds(20));
#if defined(_WIN32)
        DWORD errors=0; COMSTAT status{};
        if (!ClearCommError(state_->handle, &errors, &status)) return native_failure();
        if (errors) return failure(ErrorCode::system_error);
        if (interest == Interest::readable ? status.cbInQue > 0 : status.cbOutQue == 0) return {};
#else
        pollfd descriptor{state_->handle, static_cast<short>(interest == Interest::readable ? POLLIN : POLLOUT), 0};
        const auto ready=::poll(&descriptor, 1, static_cast<int>(slice.count()));
        if (ready < 0) { if (errno == EINTR) continue; return native_failure(); }
        if (descriptor.revents & POLLNVAL) return failure(ErrorCode::closed);
        if (descriptor.revents & (descriptor.events | (interest == Interest::readable ? POLLHUP : 0))) return {};
        if (descriptor.revents & (POLLERR | POLLHUP)) return failure(ErrorCode::system_error);
#endif
        if (stop.cancelled()) return failure(ErrorCode::cancelled);
        if (remaining.count() == 0) return failure(timeout.count() == 0 ? ErrorCode::would_block : ErrorCode::timed_out);
#if defined(_WIN32)
        Sleep(static_cast<DWORD>(slice.count()));
#endif
    }
}
} // namespace ii::io
