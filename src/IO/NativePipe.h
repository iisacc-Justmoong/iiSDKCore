#pragma once
#include "Channel.h"

namespace ii::io {
// Platform integration only, like NativeFile. Takes ownership even on failure
// or allocation exception. Requires a POSIX FIFO or Windows byte-mode pipe;
// makes the owned endpoint nonblocking. No native handle escapes Channel.
class NativePipe final {
public:
    [[nodiscard]] static Result<Channel> adopt(std::intptr_t owned_handle, Access access);
};
} // namespace ii::io
