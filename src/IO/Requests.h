#pragma once

#include "Channel.h"
#include <variant>
#include <vector>

namespace ii::io {

// Copies share fixed storage. Access it only after transfer completion.
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
struct Read final {
    Buffer buffer;
    std::size_t offset = 0, length = std::dynamic_extent;
    std::chrono::milliseconds timeout{0};
    CancellationToken stop{};
};
struct Write final {
    Buffer buffer;
    std::size_t offset = 0, length = std::dynamic_extent;
    std::chrono::milliseconds timeout{0};
    CancellationToken stop{};
};
struct Seek final { std::int64_t offset; SeekOrigin origin = SeekOrigin::begin; };
struct Flush final {};
struct Close final {};
using IOQuery = std::variant<Read, Write, Seek, Flush, Close>;
struct IOResponse final { Buffer buffer; Transfer transfer; std::uint64_t position = 0; };
using IOResult = Result<IOResponse>;

} // namespace ii::io
