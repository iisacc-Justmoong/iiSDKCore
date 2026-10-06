#include "DependencyGraph.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace ii::graph {
namespace { std::atomic<std::uint64_t> next_owner{1}; }
struct DependencyGraph::Impl {
    struct Slot { std::uint64_t generation=0; bool active=false; std::vector<std::size_t> outgoing; };
    mutable std::mutex mutex;
    const std::uint64_t owner=next_owner.fetch_add(1,std::memory_order_relaxed);
    std::vector<Slot> slots;
    std::vector<std::size_t> free;
    explicit Impl(std::size_t capacity) : slots(capacity) {
        if (!capacity) throw std::invalid_argument("Graph capacity must be positive");
        free.reserve(capacity);
        for (std::size_t i=capacity;i>0;--i) free.push_back(i-1);
    }
    bool valid(NodeId id) const { return id.owner==owner && id.slot<slots.size() && slots[id.slot].active && slots[id.slot].generation==id.generation; }
    NodeId id(std::size_t slot) const { return {owner,slots[slot].generation,slot}; }
};
DependencyGraph::DependencyGraph(std::size_t capacity) : impl_(std::make_unique<Impl>(capacity)) {}
DependencyGraph::~DependencyGraph()=default;
core::Result<NodeId> DependencyGraph::add() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->free.empty()) return core::fail(core::ErrorCode::full,"Graph node capacity reached");
    const auto index=impl_->free.back();
    auto& slot=impl_->slots[index];
    if (slot.generation==std::numeric_limits<std::uint64_t>::max()) return core::fail(core::ErrorCode::out_of_range,"Node generation exhausted");
    impl_->free.pop_back(); ++slot.generation; slot.active=true;
    return impl_->id(index);
}
bool DependencyGraph::contains(NodeId node) const { std::lock_guard lock(impl_->mutex); return impl_->valid(node); }
core::Result<void> DependencyGraph::remove(NodeId node) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->valid(node)) return core::fail(core::ErrorCode::not_found,"Invalid graph node");
    for (auto& slot:impl_->slots) std::erase(slot.outgoing,node.slot);
    auto& slot=impl_->slots[node.slot];
    std::vector<std::size_t>{}.swap(slot.outgoing); slot.active=false;
    impl_->free.push_back(node.slot); return {};
}
core::Result<void> DependencyGraph::connect(NodeId from,NodeId to) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->valid(from) || !impl_->valid(to)) return core::fail(core::ErrorCode::not_found,"Invalid graph endpoint");
    auto& outgoing=impl_->slots[from.slot].outgoing;
    if (std::find(outgoing.begin(),outgoing.end(),to.slot)!=outgoing.end()) return core::fail(core::ErrorCode::already_exists,"Edge already exists");
    std::vector<bool> visited(impl_->slots.size());
    std::vector<std::size_t> pending{to.slot};
    while (!pending.empty()) {
        const auto current=pending.back(); pending.pop_back();
        if (current==from.slot) return core::fail(core::ErrorCode::cycle,"Dependency cycle rejected");
        if (visited[current]) continue;
        visited[current]=true;
        for (auto child:impl_->slots[current].outgoing) pending.push_back(child);
    }
    outgoing.push_back(to.slot); return {};
}
core::Result<void> DependencyGraph::disconnect(NodeId from,NodeId to) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->valid(from) || !impl_->valid(to)) return core::fail(core::ErrorCode::not_found,"Invalid graph endpoint");
    if (!std::erase(impl_->slots[from.slot].outgoing,to.slot)) return core::fail(core::ErrorCode::not_found,"Edge not found");
    return {};
}
core::Result<std::vector<NodeId>> DependencyGraph::children(NodeId node) const {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->valid(node)) return core::fail(core::ErrorCode::not_found,"Invalid graph node");
    std::vector<NodeId> result;
    for (auto child:impl_->slots[node.slot].outgoing) result.push_back(impl_->id(child));
    return result;
}
core::Result<std::vector<NodeId>> DependencyGraph::order() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<std::size_t> incoming(impl_->slots.size());
    for (const auto& slot:impl_->slots) for (auto child:slot.outgoing) ++incoming[child];
    std::vector<NodeId> result;
    for (std::size_t i=0;i<impl_->slots.size();++i)
        if (impl_->slots[i].active && incoming[i]==0) result.push_back(impl_->id(i));
    for (std::size_t i=0;i<result.size();++i)
        for (auto child:impl_->slots[result[i].slot].outgoing)
            if (--incoming[child]==0) result.push_back(impl_->id(child));
    return result;
}
} // namespace ii::graph
