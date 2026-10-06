#pragma once

#include "Requests.h"
#include "../Runtime/Query.h"

namespace ii::io {

// Compose a per-channel serial execution domain on persistent runtime workers.
// Long/blocking I/O should use a dedicated Runtime so CPU work keeps progressing.
class AsyncIO final {
public:
    AsyncIO(runtime::Runtime& runtime, Channel channel);
    ~AsyncIO();
    [[nodiscard]] runtime::ThreadResult close();
    [[nodiscard]] const runtime::QueryPort<IOQuery, IOResult>& queries() const noexcept { return queries_; }
private:
    runtime::SerialQueue queue_;
    runtime::QueryPort<IOQuery, IOResult> queries_;
};
} // namespace ii::io
