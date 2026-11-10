#include "ManagedMemory.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>

namespace ii::runtime {
namespace detail {
struct ManagedMemoryState final {
    explicit ManagedMemoryState(std::size_t limit){stats.limit=limit;}
    mutable std::mutex mutex;
    MemoryStats stats;
};
}
MemoryPool::MemoryPool(std::size_t limit):state_(std::make_shared<detail::ManagedMemoryState>(limit)){}
MemoryPool::MemoryPool(std::shared_ptr<detail::ManagedMemoryState> state) noexcept:state_(std::move(state)){}
MemoryStats MemoryPool::stats() const {
    if(!state_)return {};
    std::lock_guard lock(state_->mutex);return state_->stats;
}
std::expected<void,MemoryError> MemoryPool::set_limit(std::size_t bytes) const {
    if(!state_)return std::unexpected(MemoryError::closed);
    std::lock_guard lock(state_->mutex);
    if(bytes<state_->stats.used)return std::unexpected(MemoryError::limit_exceeded);
    state_->stats.limit=bytes;return {};
}
std::expected<MemoryBlock,MemoryError> MemoryPool::allocate(std::size_t bytes,std::size_t alignment) const {
    if(!state_)return std::unexpected(MemoryError::closed);
    if(!bytes||!std::has_single_bit(alignment)||alignment<alignof(std::max_align_t))
        return std::unexpected(MemoryError::invalid);
    {
        std::lock_guard lock(state_->mutex);auto& stats=state_->stats;
        if(bytes>stats.limit-stats.used)return std::unexpected(MemoryError::limit_exceeded);
        stats.used+=bytes;++stats.allocations;stats.peak=std::max(stats.peak,stats.used);
    }
    std::byte* allocation=nullptr;
    try { allocation=static_cast<std::byte*>(::operator new(bytes,std::align_val_t{alignment})); }
    catch(const std::bad_alloc&) {
        std::lock_guard lock(state_->mutex);state_->stats.used-=bytes;--state_->stats.allocations;
        return std::unexpected(MemoryError::out_of_memory);
    }
    return MemoryBlock(state_,allocation,bytes,alignment);
}
MemoryBlock::MemoryBlock(std::shared_ptr<detail::ManagedMemoryState> state,std::byte* data,std::size_t size,std::size_t alignment) noexcept
    :state_(std::move(state)),data_(data),size_(size),alignment_(alignment){}
MemoryBlock::~MemoryBlock(){reset();}
MemoryBlock::MemoryBlock(MemoryBlock&& other) noexcept
    :state_(std::move(other.state_)),data_(std::exchange(other.data_,nullptr)),
     size_(std::exchange(other.size_,0)),alignment_(std::exchange(other.alignment_,0)){}
MemoryBlock& MemoryBlock::operator=(MemoryBlock&& other) noexcept {
    if(this==&other)return *this;
    reset();state_=std::move(other.state_);data_=std::exchange(other.data_,nullptr);
    size_=std::exchange(other.size_,0);alignment_=std::exchange(other.alignment_,0);return *this;
}
void MemoryBlock::reset() noexcept {
    if(!data_)return;
    auto state=std::move(state_);auto* data=std::exchange(data_,nullptr);
    const auto size=std::exchange(size_,0),alignment=std::exchange(alignment_,0);
    ::operator delete(data,std::align_val_t{alignment});
    std::lock_guard lock(state->mutex);state->stats.used-=size;--state->stats.allocations;
}
std::expected<void,MemoryError> MemoryBlock::resize(std::size_t bytes) {
    if(!data_)return std::unexpected(MemoryError::closed);
    if(bytes==size_)return {};
    auto next=MemoryPool(state_).allocate(bytes,alignment_);
    if(!next)return std::unexpected(next.error());
    std::memcpy(next->data_,data_,std::min(size_,bytes));*this=std::move(*next);return {};
}
std::expected<void,MemoryError> MemoryBlock::transfer(const MemoryPool& destination) {
    if(!data_||!destination.state_)return std::unexpected(MemoryError::closed);
    if(state_==destination.state_)return {};
    auto next=destination.allocate(size_,alignment_);
    if(!next)return std::unexpected(next.error());
    std::memcpy(next->data_,data_,size_);*this=std::move(*next);return {};
}
} // namespace ii::runtime
