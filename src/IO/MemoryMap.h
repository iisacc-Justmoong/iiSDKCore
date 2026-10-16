#pragma once
#include "Channel.h"

namespace ii::io {
enum class MappingBackend { unavailable, posix, windows };
enum class AccessPattern { normal, sequential, random };

// Immutable, shared native view. Closing the source Channel does not invalidate
// existing views. The backing file must not be modified/truncated in place.
class MappedRegion final {
public:
    MappedRegion() = default;
    [[nodiscard]] static bool supported() noexcept;
    [[nodiscard]] static Result<MappedRegion> map(const Channel&,
        std::uint64_t offset = 0, std::size_t length = std::dynamic_extent);
    [[nodiscard]] std::span<const std::byte> bytes() const & noexcept;
    std::span<const std::byte> bytes() const && = delete;
    [[nodiscard]] MappingBackend backend() const noexcept;
    [[nodiscard]] Result<MappedRegion> slice(std::size_t offset,
        std::size_t length = std::dynamic_extent) const;
    // Paging hints are observable requests, not residency/latency guarantees.
    [[nodiscard]] Result<void> prefetch() const;
    [[nodiscard]] Result<void> advise(AccessPattern) const;
private:
    struct State;
    std::shared_ptr<State> state_;
    std::size_t offset_ = 0, length_ = 0;
};
} // namespace ii::io
