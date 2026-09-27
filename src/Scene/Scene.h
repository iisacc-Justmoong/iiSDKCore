#pragma once
#include "../Graph/DependencyGraph.h"
#include "../Runtime/ObjectProperties.h"

namespace ii::scene {
// A hierarchy of instance-owned properties, not a renderer or game-specific world.
// Mutations and borrowed property access belong to one serial execution domain.
class Scene final {
public:
    explicit Scene(runtime::SerialQueue& queue,std::size_t capacity=256);
    ~Scene();
    Scene(const Scene&)=delete;
    Scene& operator=(const Scene&)=delete;
    [[nodiscard]] core::Result<graph::NodeId> create(std::vector<runtime::PropertyDefinition> properties,
                                                   std::optional<graph::NodeId> parent={});
    [[nodiscard]] core::Result<void> reparent(graph::NodeId node,std::optional<graph::NodeId> parent);
    [[nodiscard]] core::Result<void> erase(graph::NodeId subtree);
    [[nodiscard]] core::Result<std::reference_wrapper<runtime::ObjectProperties>> properties(graph::NodeId node);
    [[nodiscard]] core::Result<std::optional<graph::NodeId>> parent(graph::NodeId node) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ii::scene
