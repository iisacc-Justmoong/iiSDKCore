#include "NativeFile.h"
#include "NativePipe.h"
#include "Channel.h"
#include "Poller.h"
#include "MemoryMap.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <vector>
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
#include <sys/mman.h>
#include <termios.h>
#include <pthread.h>
#include <signal.h>
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
    case ERROR_BROKEN_PIPE: case ERROR_PIPE_NOT_CONNECTED: code=ErrorCode::closed; break;
    case ERROR_NO_DATA: case ERROR_PIPE_BUSY: code=ErrorCode::would_block; break;
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
    case EBADF: case EPIPE: code=ErrorCode::closed; break;
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
    return {Backend::posix, true, false, true};
#else
    return {Backend::posix, true, true, true};
#endif
}

struct Channel::State {
    mutable std::mutex mutex;
    Capabilities caps;
    std::filesystem::path notificationPath;
    NativeFile::Closed closed = nullptr;
    int completedClose(int result) {
        if (result == 0 && closed) closed(notificationPath);
        closed = nullptr; notificationPath.clear(); return result;
    }
#if defined(_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool valid() const { return handle != INVALID_HANDLE_VALUE; }
    int release() { auto old=handle; handle=INVALID_HANDLE_VALUE; return completedClose(CloseHandle(old) ? 0 : -1); }
#else
    int handle = -1;
    bool valid() const { return handle >= 0; }
    int release() { auto old=handle; handle=-1; return completedClose(::close(old)); } // Never retry close after EINTR.
#endif
    ~State() { if (valid()) (void)release(); }
    Result<std::unique_lock<std::mutex>> lock() {
        std::unique_lock held(mutex, std::try_to_lock);
        if (!held) return failure(ErrorCode::busy);
        if (!valid()) return failure(ErrorCode::closed);
        return held;
    }
};

struct MappedRegion::State final {
    void* base = nullptr;
    std::size_t size = 0;
#if defined(_WIN32)
    HANDLE mapping = nullptr;
#endif
    ~State() {
#if defined(_WIN32)
        if (base) UnmapViewOfFile(base);
        if (mapping) CloseHandle(mapping);
#elif !defined(__EMSCRIPTEN__)
        if (base) ::munmap(base,size);
#endif
    }
};
bool MappedRegion::supported() noexcept {
#if defined(__EMSCRIPTEN__)
    return false;
#else
    return true;
#endif
}
Result<MappedRegion> MappedRegion::map(const Channel& source,std::uint64_t offset,std::size_t length) {
#if defined(__EMSCRIPTEN__)
    (void)source;(void)offset;(void)length;
    return failure(ErrorCode::unsupported);
#else
    if (!source.state_) return failure(ErrorCode::closed);
    auto locked=source.state_->lock();if (!locked) return std::unexpected(locked.error());
    if (source.state_->caps.kind!=ChannelKind::file || !source.state_->caps.readable)
        return failure(ErrorCode::unsupported);
    std::uint64_t file_size=0,granularity=0;
#if defined(_WIN32)
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(source.state_->handle,&size)) return native_failure();
    if (size.QuadPart<0) return failure(ErrorCode::invalid_argument);
    file_size=static_cast<std::uint64_t>(size.QuadPart);
    SYSTEM_INFO system{};GetSystemInfo(&system);granularity=system.dwAllocationGranularity;
#else
    struct stat info{};
    if (fstat(source.state_->handle,&info)!=0) return native_failure();
    if (info.st_size<0) return failure(ErrorCode::invalid_argument);
    file_size=static_cast<std::uint64_t>(info.st_size);
    const auto page=::sysconf(_SC_PAGESIZE);
    if (page<=0) return failure(ErrorCode::unsupported);
    granularity=static_cast<std::uint64_t>(page);
#endif
    if (!granularity || offset>file_size) return failure(ErrorCode::invalid_argument);
    const auto available=file_size-offset;
    if (length==std::dynamic_extent) {
        if (available>std::numeric_limits<std::size_t>::max()) return failure(ErrorCode::invalid_argument);
        length=static_cast<std::size_t>(available);
    }
    if (length>available) return failure(ErrorCode::invalid_argument);
    const auto aligned=offset-offset%granularity,delta=offset-aligned;
    if (delta>std::numeric_limits<std::size_t>::max()-length) return failure(ErrorCode::invalid_argument);
    auto owner=std::make_shared<State>();
    MappedRegion result;result.state_=owner;result.length_=length;
    if (!length) return result;
    result.offset_=static_cast<std::size_t>(delta);
    owner->size=static_cast<std::size_t>(delta)+length;
#if defined(_WIN32)
    owner->mapping=CreateFileMappingW(source.state_->handle,nullptr,PAGE_READONLY,0,0,nullptr);
    if (!owner->mapping) return native_failure();
    owner->base=MapViewOfFile(owner->mapping,FILE_MAP_READ,static_cast<DWORD>(aligned>>32),
        static_cast<DWORD>(aligned),owner->size);
    if (!owner->base) return native_failure();
#else
    if (aligned>static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        return failure(ErrorCode::invalid_argument);
    owner->base=::mmap(nullptr,owner->size,PROT_READ,MAP_PRIVATE,source.state_->handle,static_cast<off_t>(aligned));
    if (owner->base==MAP_FAILED) { owner->base=nullptr;return native_failure(); }
#endif
    return result;
#endif
}
std::span<const std::byte> MappedRegion::bytes() const & noexcept {
    if (!state_ || !state_->base) return {};
    return {static_cast<const std::byte*>(state_->base)+offset_,length_};
}
MappingBackend MappedRegion::backend() const noexcept {
    if (!state_) return MappingBackend::unavailable;
#if defined(_WIN32)
    return MappingBackend::windows;
#elif !defined(__EMSCRIPTEN__)
    return MappingBackend::posix;
#else
    return MappingBackend::unavailable;
#endif
}
Result<MappedRegion> MappedRegion::slice(std::size_t offset,std::size_t length) const {
    if (!state_) return failure(ErrorCode::closed);
    if (offset>length_) return failure(ErrorCode::invalid_argument);
    if (length==std::dynamic_extent) length=length_-offset;
    if (length>length_-offset) return failure(ErrorCode::invalid_argument);
    auto result=*this;result.offset_+=offset;result.length_=length;return result;
}
Result<void> MappedRegion::prefetch() const {
    if (!state_) return failure(ErrorCode::closed);
    if (!length_) return {};
#if defined(_WIN32) && _WIN32_WINNT >= 0x0602
    // Resolve the optional paging hint without adding a newer loader import.
    using Prefetch=decltype(&PrefetchVirtualMemory);
    const auto kernel=GetModuleHandleW(L"kernel32.dll");
    if (!kernel) return native_failure();
    const auto prefetch=reinterpret_cast<Prefetch>(GetProcAddress(kernel,"PrefetchVirtualMemory"));
    if (!prefetch) return failure(ErrorCode::unsupported);
    WIN32_MEMORY_RANGE_ENTRY range{static_cast<std::byte*>(state_->base)+offset_,length_};
    if (!prefetch(GetCurrentProcess(),1,&range,0)) return native_failure();
    return {};
#elif !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    const auto page=::sysconf(_SC_PAGESIZE);if (page<=0) return failure(ErrorCode::unsupported);
    const auto delta=offset_%static_cast<std::size_t>(page);
    if (::madvise(static_cast<std::byte*>(state_->base)+offset_-delta,length_+delta,MADV_WILLNEED)!=0)
        return native_failure();
    return {};
#else
    return failure(ErrorCode::unsupported);
#endif
}
Result<void> MappedRegion::advise(AccessPattern pattern) const {
    if (!state_) return failure(ErrorCode::closed);
    if (pattern!=AccessPattern::normal && pattern!=AccessPattern::random && pattern!=AccessPattern::sequential)
        return failure(ErrorCode::invalid_argument);
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    if (!length_) return {};
    const auto page=::sysconf(_SC_PAGESIZE);if (page<=0) return failure(ErrorCode::unsupported);
    const auto delta=offset_%static_cast<std::size_t>(page);
    const auto advice=pattern==AccessPattern::normal?MADV_NORMAL:pattern==AccessPattern::random?MADV_RANDOM:MADV_SEQUENTIAL;
    if (::madvise(static_cast<std::byte*>(state_->base)+offset_-delta,length_+delta,advice)!=0) return native_failure();
    return {};
#else
    return failure(ErrorCode::unsupported);
#endif
}

Result<Channel> NativeFile::adopt(std::intptr_t owned_handle, Access access,
        std::filesystem::path notification_path, Closed closed) {
    struct Owner final {
        std::intptr_t handle;
        bool released = false;
        ~Owner() {
            if (released) return;
#if defined(_WIN32)
            if (handle && reinterpret_cast<HANDLE>(handle) != INVALID_HANDLE_VALUE) CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
            if (handle >= 0 && handle <= std::numeric_limits<int>::max()) ::close(static_cast<int>(handle));
#endif
        }
    } owner{owned_handle};
    auto state = std::make_shared<Channel::State>();
#if defined(_WIN32)
    state->handle = reinterpret_cast<HANDLE>(owned_handle);
    if (!state->valid() || state->handle == nullptr) return failure(ErrorCode::invalid_argument);
#else
    if (owned_handle < 0 || owned_handle > std::numeric_limits<int>::max()) return failure(ErrorCode::invalid_argument);
    state->handle = static_cast<int>(owned_handle);
#endif
    owner.released = true;
    if (access != Access::read && access != Access::write && access != Access::read_write)
        return failure(ErrorCode::invalid_argument);
    state->caps = {ChannelKind::file, platform_capabilities().backend,
        access != Access::write, access != Access::read, true, access != Access::read};
#if defined(_WIN32)
    if (GetFileType(state->handle) != FILE_TYPE_DISK) return failure(ErrorCode::unsupported);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(state->handle, &info)) return native_failure();
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return failure(ErrorCode::unsupported);
#else
    struct stat info{};
    if (fstat(state->handle, &info) != 0) return native_failure();
    if (!S_ISREG(info.st_mode)) return failure(ErrorCode::unsupported);
#endif
    state->notificationPath = std::move(notification_path); state->closed = closed;
    return Channel(std::move(state));
}

Result<Channel> NativePipe::adopt(std::intptr_t owned_handle, Access access) {
    struct Owner final {
        std::intptr_t handle;
        bool released = false;
        ~Owner() {
            if (released) return;
#if defined(_WIN32)
            if (handle && reinterpret_cast<HANDLE>(handle) != INVALID_HANDLE_VALUE) CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
            if (handle >= 0 && handle <= std::numeric_limits<int>::max()) ::close(static_cast<int>(handle));
#endif
        }
    } owner{owned_handle};
    auto state = std::make_shared<Channel::State>();
#if defined(_WIN32)
    state->handle = reinterpret_cast<HANDLE>(owned_handle);
    if (!state->valid() || !state->handle) return failure(ErrorCode::invalid_argument);
#else
    if (owned_handle < 0 || owned_handle > std::numeric_limits<int>::max()) return failure(ErrorCode::invalid_argument);
    state->handle = static_cast<int>(owned_handle);
#endif
    owner.released = true;
    if (access != Access::read && access != Access::write && access != Access::read_write)
        return failure(ErrorCode::invalid_argument);
    state->caps = {ChannelKind::pipe, platform_capabilities().backend,
        access != Access::write, access != Access::read, false, false};
#if defined(_WIN32)
    if (GetFileType(state->handle) != FILE_TYPE_PIPE) return failure(ErrorCode::unsupported);
    DWORD mode=PIPE_READMODE_BYTE | PIPE_NOWAIT;
    DWORD current_mode=0;
    if (access==Access::read && !GetNamedPipeHandleStateW(state->handle,&current_mode,nullptr,nullptr,nullptr,nullptr,0))
        return native_failure();
    // Read-only server handles need FILE_WRITE_ATTRIBUTES to change mode, even
    // when the requested byte/nonblocking mode was already set at creation.
    if ((access!=Access::read || (current_mode&(PIPE_READMODE_MESSAGE|PIPE_NOWAIT))!=mode)
        && !SetNamedPipeHandleState(state->handle,&mode,nullptr,nullptr)) return native_failure();
#elif !defined(__EMSCRIPTEN__)
    struct stat info{};
    if (fstat(state->handle,&info)!=0) return native_failure();
    if (!S_ISFIFO(info.st_mode)) return failure(ErrorCode::unsupported);
    const int flags=fcntl(state->handle,F_GETFL);
    if (flags<0 || fcntl(state->handle,F_SETFL,flags|O_NONBLOCK)<0 || fcntl(state->handle,F_SETFD,FD_CLOEXEC)<0)
        return native_failure();
#if defined(__APPLE__) && defined(F_SETNOSIGPIPE)
    if (state->caps.writable && fcntl(state->handle,F_SETNOSIGPIPE,1)!=0) return native_failure();
#endif
#else
    return failure(ErrorCode::unsupported);
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

Result<Channel> open_device(const std::filesystem::path& path, DeviceOptions options) {
    if (!platform_capabilities().devices) return failure(ErrorCode::unsupported);
    if (!valid_path(path) || (options.access!=Access::read && options.access!=Access::write &&
        options.access!=Access::read_write)) return failure(ErrorCode::invalid_argument);
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    auto state=std::make_shared<Channel::State>();
    state->caps={ChannelKind::device,Backend::posix,options.access!=Access::write,
        options.access!=Access::read,false,false};
    const int mode=options.access==Access::read ? O_RDONLY : options.access==Access::write ? O_WRONLY : O_RDWR;
    do { state->handle=::open(path.c_str(),mode|O_NONBLOCK|O_CLOEXEC|O_NOCTTY); }
    while (!state->valid() && errno==EINTR);
    if (!state->valid()) return native_failure();
    struct stat info{};
    if (fstat(state->handle,&info)!=0) return native_failure();
    if (!S_ISCHR(info.st_mode) && !S_ISFIFO(info.st_mode)) return failure(ErrorCode::unsupported);
#if defined(__APPLE__) && defined(F_SETNOSIGPIPE)
    // Darwin may deliver a pipe's process signal to another unblocked worker.
    // Suppress it on this FIFO rather than changing the host's signal handler.
    if (S_ISFIFO(info.st_mode) && state->caps.writable && fcntl(state->handle,F_SETNOSIGPIPE,1)!=0)
        return native_failure();
#endif
    return Channel(std::move(state));
#else
    return failure(ErrorCode::unsupported);
#endif
}

Capabilities Channel::capabilities() const noexcept { return state_ ? state_->caps : Capabilities{}; }
Result<Transfer> Channel::read(std::span<std::byte> destination) const {
    if (!state_) return failure(ErrorCode::closed);
    auto held = state_->lock(); if (!held) return std::unexpected(held.error());
    if (!state_->caps.readable) return failure(ErrorCode::permission_denied);
    if (destination.empty()) return Transfer{};
#if defined(_WIN32)
    if (state_->caps.kind==ChannelKind::pipe) {
        DWORD available=0;
        if (!PeekNamedPipe(state_->handle,nullptr,0,nullptr,&available,nullptr)) {
            const auto error=GetLastError();
            if (error==ERROR_BROKEN_PIPE || error==ERROR_PIPE_NOT_CONNECTED) return Transfer{0,true};
            return native_failure();
        }
        if (!available) return failure(ErrorCode::would_block);
    }
    DWORD count=0;
    if (!ReadFile(state_->handle, destination.data(), static_cast<DWORD>(std::min<std::size_t>(destination.size(), MAXDWORD)), &count, nullptr)) {
        const auto error=GetLastError();
        if (state_->caps.kind==ChannelKind::pipe && (error==ERROR_BROKEN_PIPE || error==ERROR_PIPE_NOT_CONNECTED)) return Transfer{0,true};
        return native_failure();
    }
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
    if (!WriteFile(state_->handle, source.data(), static_cast<DWORD>(std::min<std::size_t>(source.size(), MAXDWORD)), &count, nullptr)) {
        const auto error=GetLastError();
        if (state_->caps.kind==ChannelKind::pipe && error==ERROR_NO_DATA)
            return failure(ErrorCode::closed,{static_cast<int>(error),std::system_category()});
        return native_failure();
    }
    if (!count && state_->caps.kind==ChannelKind::pipe) return failure(ErrorCode::would_block);
#else
    ssize_t count;
#if !defined(__EMSCRIPTEN__)
    // A disconnected FIFO must report EPIPE without terminating the host.
    // Block/consume only this thread's newly generated SIGPIPE, preserving the
    // host's process handler and any signal that was already pending.
    sigset_t blocked{}, previous{}, pending{};
    const bool device=state_->caps.kind==ChannelKind::device || state_->caps.kind==ChannelKind::pipe;
    bool was_pending=false;
    if (device) {
        sigemptyset(&blocked); sigaddset(&blocked,SIGPIPE);
        const int error=pthread_sigmask(SIG_BLOCK,&blocked,&previous);
        if (error) { errno=error; return native_failure(); }
        sigpending(&pending); was_pending=sigismember(&pending,SIGPIPE)==1;
    }
#endif
    do { count=::write(state_->handle, source.data(), std::min(source.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()))); } while (count < 0 && errno == EINTR);
#if !defined(__EMSCRIPTEN__)
    const int error=errno;
    if (device) {
        if (count<0 && error==EPIPE && !was_pending) {
            sigpending(&pending);
            if (sigismember(&pending,SIGPIPE)==1) {
                int signal=0, result;
                do { result=sigwait(&blocked,&signal); } while (result==EINTR);
            }
        }
        (void)pthread_sigmask(SIG_SETMASK,&previous,nullptr);
    }
    errno=error;
#endif
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
        if (state_->caps.kind==ChannelKind::pipe) {
            // Byte-mode PIPE_NOWAIT writes are the authoritative readiness
            // probe. Win32 exposes no writable-space notification for these.
            if (interest==Interest::writable) return {};
            DWORD available=0;
            if (!PeekNamedPipe(state_->handle,nullptr,0,nullptr,&available,nullptr)) {
                if (GetLastError()==ERROR_BROKEN_PIPE || GetLastError()==ERROR_PIPE_NOT_CONNECTED) return {};
                return native_failure();
            }
            if (available) return {};
        } else {
            DWORD errors=0; COMSTAT status{};
            if (!ClearCommError(state_->handle, &errors, &status)) return native_failure();
            if (errors) return failure(ErrorCode::system_error);
            if (interest == Interest::readable ? status.cbInQue > 0 : status.cbOutQue == 0) return {};
        }
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

namespace detail {
struct ReadinessPoller::Impl {
    explicit Impl(std::size_t capacity) {
#if defined(_WIN32)
        (void)capacity;
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if (!event) throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"I/O wake event");
#elif !defined(__EMSCRIPTEN__)
        leases.reserve(capacity); descriptors.reserve(capacity+1);
        int pipe_handles[2];
        if (::pipe(pipe_handles)!=0) throw std::system_error(errno,std::generic_category(),"I/O wake pipe");
        wake_read=pipe_handles[0]; wake_write=pipe_handles[1];
        if (fcntl(wake_read,F_SETFD,FD_CLOEXEC)<0 || fcntl(wake_write,F_SETFD,FD_CLOEXEC)<0 ||
            fcntl(wake_read,F_SETFL,O_NONBLOCK)<0 || fcntl(wake_write,F_SETFL,O_NONBLOCK)<0) {
            const int error=errno; ::close(wake_read); ::close(wake_write);
            throw std::system_error(error,std::generic_category(),"I/O wake pipe flags");
        }
#else
        (void)capacity;
#endif
    }
    ~Impl() {
#if defined(_WIN32)
        if (event) CloseHandle(event);
#elif !defined(__EMSCRIPTEN__)
        ::close(wake_read); ::close(wake_write);
#endif
    }
    void finish() {
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
        leases.clear(); // unlock handles and drop native owning references first
        descriptors.clear();
#endif
        active.store(false,std::memory_order_release);
        { std::lock_guard lock(mutex); finished=prepared; }
        changed.notify_all();
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> active{false};
    std::uint64_t prepared=0, finished=0;
    std::chrono::milliseconds timeout{20};
#if defined(_WIN32)
    HANDLE event=nullptr;
#elif !defined(__EMSCRIPTEN__)
    struct Lease {
        std::shared_ptr<Channel::State> state;
        std::unique_lock<std::mutex> lock;
    };
    std::vector<Lease> leases;
    std::vector<pollfd> descriptors;
    int wake_read=-1,wake_write=-1;
#else
    bool signaled=false;
#endif
};
ReadinessPoller::ReadinessPoller(std::size_t capacity) : impl_(std::make_unique<Impl>(capacity)) {}
ReadinessPoller::~ReadinessPoller()=default;
void ReadinessPoller::prepare(std::span<const WatchEntry> watches,std::chrono::milliseconds timeout) {
    auto& impl=*impl_;
    impl.timeout=std::clamp(timeout,std::chrono::milliseconds(0),std::chrono::milliseconds(20));
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    impl.descriptors.clear(); impl.leases.clear();
    impl.descriptors.push_back({impl.wake_read,POLLIN,0});
    for (const auto& watch:watches) {
        const auto state=watch.channel.state_;
        if (!state) continue;
        const short interest=watch.interest==Interest::readable ? POLLIN : POLLOUT;
        const auto existing=std::find_if(impl.leases.begin(),impl.leases.end(),
            [&](const auto& lease){return lease.state==state;});
        if (existing!=impl.leases.end()) {
            impl.descriptors[static_cast<std::size_t>(existing-impl.leases.begin())+1].events|=interest;
            continue;
        }
        std::unique_lock held(state->mutex,std::try_to_lock);
        if (!held || !state->valid()) continue;
        impl.descriptors.push_back({state->handle,interest,0});
        impl.leases.push_back({state,std::move(held)});
    }
#else
    (void)watches;
#endif
    { std::lock_guard lock(impl.mutex); ++impl.prepared; }
    impl.active.store(true,std::memory_order_release);
}
void ReadinessPoller::wait() {
    auto& impl=*impl_;
    try {
#if defined(_WIN32)
        if (WaitForSingleObject(impl.event,static_cast<DWORD>(impl.timeout.count()))==WAIT_FAILED)
            throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"I/O event wait");
#elif !defined(__EMSCRIPTEN__)
        const auto result=::poll(impl.descriptors.data(),static_cast<nfds_t>(impl.descriptors.size()),
            static_cast<int>(impl.timeout.count()));
        if (result<0 && errno!=EINTR) throw std::system_error(errno,std::generic_category(),"I/O poll");
        std::array<char,64> bytes{};
        for (;;) {
            const auto count=::read(impl.wake_read,bytes.data(),bytes.size());
            if (count<0 && errno==EINTR) continue;
            if (count<=0) break;
        }
#else
        std::unique_lock lock(impl.mutex);
        impl.changed.wait_for(lock,impl.timeout,[&]{return impl.signaled;});
        impl.signaled=false;
#endif
    } catch (...) { impl.finish(); throw; }
    impl.finish();
}
void ReadinessPoller::wake() noexcept {
    auto& impl=*impl_;
    if (!impl.active.load(std::memory_order_acquire)) return;
#if defined(_WIN32)
    (void)SetEvent(impl.event);
#elif !defined(__EMSCRIPTEN__)
    const char byte=1;
    ssize_t count;
    do { count=::write(impl.wake_write,&byte,1); } while (count<0 && errno==EINTR);
#else
    { std::lock_guard lock(impl.mutex); impl.signaled=true; }
    impl.changed.notify_all();
#endif
}
void ReadinessPoller::synchronize() {
    auto& impl=*impl_;
    std::unique_lock lock(impl.mutex);
    const auto epoch=impl.prepared;
    impl.changed.wait(lock,[&]{return impl.finished>=epoch;});
}
} // namespace detail
} // namespace ii::io
