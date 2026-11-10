#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <span>

namespace ii::runtime {
enum class MemoryError { invalid, limit_exceeded, out_of_memory, closed };
struct MemoryStats final {
    std::size_t limit=0,used=0,peak=0,allocations=0;
};
namespace detail { struct ManagedMemoryState; }
class MemoryPool;

// The block owns its allocation, and may outlive the MemoryPool handle.
// Spans are invalidated by resize, transfer, reset, move assignment or destruction.
class MemoryBlock final {
public:
    MemoryBlock() noexcept=default;
    ~MemoryBlock();
    MemoryBlock(MemoryBlock&&) noexcept;
    MemoryBlock& operator=(MemoryBlock&&) noexcept;
    MemoryBlock(const MemoryBlock&)=delete;
    MemoryBlock& operator=(const MemoryBlock&)=delete;
    explicit operator bool() const noexcept { return data_!=nullptr; }
    std::size_t size() const noexcept { return size_; }
    std::size_t alignment() const noexcept { return alignment_; }
    std::span<std::byte> bytes() & noexcept { return {data_,size_}; }
    std::span<const std::byte> bytes() const & noexcept { return {data_,size_}; }
    std::span<std::byte> bytes() && =delete;
    std::span<const std::byte> bytes() const && =delete;
    [[nodiscard]] std::expected<void,MemoryError> resize(std::size_t bytes);
    [[nodiscard]] std::expected<void,MemoryError> transfer(const MemoryPool& destination);
    void reset() noexcept;
private:
    friend class MemoryPool;
    MemoryBlock(std::shared_ptr<detail::ManagedMemoryState>,std::byte*,std::size_t,std::size_t) noexcept;
    std::shared_ptr<detail::ManagedMemoryState> state_;
    std::byte* data_=nullptr;
    std::size_t size_=0,alignment_=0;
};

// Copies refer to the same pool. Distinct blocks can be allocated/released on
// different threads; concurrent operations on one block require external synchronization.
class MemoryPool final {
public:
    explicit MemoryPool(std::size_t limit);
    [[nodiscard]] std::expected<MemoryBlock,MemoryError> allocate(
        std::size_t bytes,std::size_t alignment=alignof(std::max_align_t)) const;
    [[nodiscard]] std::expected<void,MemoryError> set_limit(std::size_t bytes) const;
    [[nodiscard]] MemoryStats stats() const;
private:
    friend class MemoryBlock;
    explicit MemoryPool(std::shared_ptr<detail::ManagedMemoryState>) noexcept;
    std::shared_ptr<detail::ManagedMemoryState> state_;
};
} // namespace ii::runtime
