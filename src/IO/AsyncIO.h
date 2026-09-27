#pragma once

#include "Channel.h"
#include "../Runtime/Query.h"
#include <variant>
#include <vector>

namespace ii::io {

// Fixed-size owning buffer reference. Copies share storage. Do not access or
// submit that storage elsewhere until the operation's completion is delivered.
class Buffer final {
public:
    Buffer() = default;
    explicit Buffer(std::size_t capacity) : storage_(std::make_shared<std::vector<std::byte>>(capacity)) {}
    [[nodiscard]] std::span<std::byte> bytes() const noexcept {
        return storage_ ? std::span<std::byte>(*storage_) : std::span<std::byte>{};
    }
private:
    std::shared_ptr<std::vector<std::byte>> storage_;
};
struct Read {
    Buffer buffer;
    std::size_t offset = 0, length = std::dynamic_extent;
    std::chrono::milliseconds timeout{0};
    CancellationToken stop{};
};
struct Write {
    Buffer buffer;
    std::size_t offset = 0, length = std::dynamic_extent;
    std::chrono::milliseconds timeout{0};
    CancellationToken stop{};
};
struct Seek { std::int64_t offset; SeekOrigin origin = SeekOrigin::begin; };
struct Flush {};
struct Close {};
using IOQuery = std::variant<Read, Write, Seek, Flush, Close>;
struct IOResponse { Buffer buffer; Transfer transfer; std::uint64_t position = 0; };
using IOResult = Result<IOResponse>;

// Compose a per-channel serial execution domain on persistent runtime workers.
// Long/blocking I/O should use a dedicated Runtime so CPU work keeps progressing.
class AsyncIO final {
public:
    AsyncIO(runtime::Runtime& runtime, Channel channel);
    [[nodiscard]] const runtime::QueryPort<IOQuery, IOResult>& queries() const noexcept { return queries_; }
private:
    runtime::SerialQueue queue_;
    runtime::QueryPort<IOQuery, IOResult> queries_;
};
} // namespace ii::io
