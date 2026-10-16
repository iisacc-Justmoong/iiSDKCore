#pragma once
#include "Channel.h"

namespace ii::io {
// Platform integration only. Ownership transfers even on failure: intptr_t is
// a POSIX descriptor or a Windows HANDLE converted by the platform adapter.
// The common Channel API never returns the native handle. A successful close
// invokes the optional noexcept notification exactly once, including RAII close.
class NativeFile final {
public:
    using Closed = void (*)(const std::filesystem::path &) noexcept;
    [[nodiscard]] static Result<Channel> adopt(std::intptr_t owned_handle, Access access,
        std::filesystem::path notification_path = {}, Closed closed = nullptr);
};
} // namespace ii::io
