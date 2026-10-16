#pragma once
// Test-only provisioning: production file creation lives in the storage module.
#include <iiSDKCore/IO/NativeFile.h>
#include <cerrno>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace ii::testing {
inline io::Result<io::Channel> open_file(const std::filesystem::path &path, io::FileOptions options = {}) {
    using namespace io;
    if (path.empty() || path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos
        || (options.access == Access::read && options.creation != Creation::existing))
        return std::unexpected(Error{ErrorCode::invalid_argument, {}});
#if defined(_WIN32)
    const DWORD access = (options.access != Access::write ? GENERIC_READ : 0) | (options.access != Access::read ? GENERIC_WRITE : 0);
    const DWORD mode = options.creation == Creation::create_new ? CREATE_NEW : options.creation == Creation::truncate
        ? CREATE_ALWAYS : options.creation == Creation::open_or_create ? OPEN_ALWAYS : OPEN_EXISTING;
    const auto handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, mode, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto number = GetLastError();
        return std::unexpected(Error{number == ERROR_FILE_NOT_FOUND ? ErrorCode::not_found
            : number == ERROR_FILE_EXISTS ? ErrorCode::already_exists : ErrorCode::system_error,
            {static_cast<int>(number), std::system_category()}});
    }
    return NativeFile::adopt(reinterpret_cast<std::intptr_t>(handle), options.access);
#else
    int flags = options.access == Access::read ? O_RDONLY : options.access == Access::write ? O_WRONLY : O_RDWR;
    if (options.creation != Creation::existing) flags |= O_CREAT;
    if (options.creation == Creation::create_new) flags |= O_EXCL;
    if (options.creation == Creation::truncate) flags |= O_TRUNC;
    const int handle = ::open(path.c_str(), flags | O_CLOEXEC | O_NONBLOCK, 0600);
    if (handle < 0) {
        const auto number = errno;
        return std::unexpected(Error{number == ENOENT ? ErrorCode::not_found : number == EEXIST
            ? ErrorCode::already_exists : ErrorCode::system_error, {number, std::generic_category()}});
    }
    return NativeFile::adopt(handle, options.access);
#endif
}
} // namespace ii::testing
