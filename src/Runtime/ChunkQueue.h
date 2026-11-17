#pragma once
#include "../Core/Result/Result.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace ii::runtime {
struct ChunkRange final { std::uint64_t offset=0; std::uint32_t count=0; };
struct ChunkDispatch final { ChunkRange range; std::uint32_t group_size=0,groups=0; };
struct ChunkDeviceLane final { std::uint32_t group_size=0,max_groups=0; };
struct ChunkQueueOptions final {
    std::size_t cpu_workers=1,capacity=64;
    std::vector<ChunkDeviceLane> devices;
    std::size_t max_concurrent_interrupts=64;
};
// wait owns the submitted operation and MUST return only after it is safe to
// release all resources. collect runs afterward unless cancellation won first.
struct ChunkSubmission final {
    std::function<core::Result<void>()> wait;
    std::function<core::Result<void>()> collect;
};
struct ChunkWork final {
    std::uint64_t items=0;
    std::uint32_t chunk_items=0;
    std::function<core::Result<void>(ChunkRange,std::stop_token)> cpu;
    std::function<core::Result<ChunkSubmission>(std::size_t,ChunkDispatch,std::stop_token)> device;
    // Synchronous on the caller of interrupt(); concurrent callers may overlap.
    std::function<core::Result<void>(std::uint64_t,std::span<const std::byte>)> interrupt;
};
enum class ChunkPhase { queued,running,pausing,paused,cancelling,completed,cancelled,failed };
struct ChunkSnapshot final {
    ChunkPhase phase=ChunkPhase::queued;
    std::uint64_t revision=0,total_items=0,claimed_items=0,completed_items=0;
    std::uint64_t completed_chunks=0,device_submitted=0;
    std::size_t cpu_active=0,device_active=0,device_in_flight=0,interrupts_active=0;
    std::optional<core::Error> error;
};
namespace detail { struct ChunkQueueState; struct ChunkTaskState; }
class ChunkTicket final {
public:
    ChunkTicket()=default;
    [[nodiscard]] ChunkSnapshot snapshot() const;
    // Waits for a different revision or timeout. Timeout never cancels work.
    [[nodiscard]] ChunkSnapshot wait_for_change(std::uint64_t,std::chrono::milliseconds) const;
    [[nodiscard]] core::Result<void> pause() const;
    [[nodiscard]] core::Result<void> resume() const;
    [[nodiscard]] core::Result<void> cancel() const;
    [[nodiscard]] core::Result<void> interrupt(std::uint64_t,std::span<const std::byte> = {}) const;
private:
    friend class ChunkQueue;
    ChunkTicket(std::shared_ptr<detail::ChunkQueueState>,std::shared_ptr<detail::ChunkTaskState>);
    std::shared_ptr<detail::ChunkQueueState> queue_;
    std::shared_ptr<detail::ChunkTaskState> task_;
};
// Bounded multi-producer queue. CPU and device lanes each claim one chunk at
// a time. close() cancels queued work and waits for accepted callbacks and
// external submissions; do not destroy/close it from one of its own callbacks.
class ChunkQueue final {
public:
    explicit ChunkQueue(ChunkQueueOptions={});
    ~ChunkQueue();
    ChunkQueue(const ChunkQueue&)=delete;
    ChunkQueue& operator=(const ChunkQueue&)=delete;
    ChunkQueue(ChunkQueue&&)=delete;
    ChunkQueue& operator=(ChunkQueue&&)=delete;
    [[nodiscard]] core::Result<ChunkTicket> submit(ChunkWork);
    [[nodiscard]] core::Result<void> close();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ii::runtime
