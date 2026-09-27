#pragma once
#include "../Core/Result/Result.h"
#include <cstdint>
#include <compare>
#include <memory>
#include <vector>

namespace ii::graph {
struct NodeId { std::uint64_t owner=0,generation=0; std::size_t slot=0; auto operator<=>(const NodeId&) const=default; };
// Fixed node slots with generation reuse; edges are a mutable acyclic graph.
// All operations synchronize internally and return value snapshots.
class DependencyGraph final {
public:
    explicit DependencyGraph(std::size_t capacity=256);
    ~DependencyGraph();
    DependencyGraph(const DependencyGraph&)=delete;
    DependencyGraph& operator=(const DependencyGraph&)=delete;
    [[nodiscard]] core::Result<NodeId> add();
    [[nodiscard]] core::Result<void> remove(NodeId node);
    [[nodiscard]] bool contains(NodeId node) const;
    [[nodiscard]] core::Result<void> connect(NodeId predecessor,NodeId successor);
    [[nodiscard]] core::Result<void> disconnect(NodeId predecessor,NodeId successor);
    [[nodiscard]] core::Result<std::vector<NodeId>> children(NodeId node) const;
    [[nodiscard]] core::Result<std::vector<NodeId>> order() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ii::graph
