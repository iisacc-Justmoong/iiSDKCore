#include "Scene.h"

namespace ii::scene {
struct Scene::Impl {
    struct Node { std::optional<graph::NodeId> parent; std::unique_ptr<runtime::ObjectProperties> properties; };
    runtime::SerialQueue& queue;
    graph::DependencyGraph graph;
    std::vector<Node> nodes;
    Impl(runtime::SerialQueue& domain,std::size_t capacity) : queue(domain),graph(capacity),nodes(capacity) {}
    core::Result<void> check(graph::NodeId node) const {
        if (!queue.is_current()) return core::fail(core::ErrorCode::wrong_executor,"Scene belongs to another execution domain");
        if (!graph.contains(node)) return core::fail(core::ErrorCode::not_found,"Scene node expired");
        return {};
    }
};
Scene::Scene(runtime::SerialQueue& queue,std::size_t capacity) : impl_(std::make_unique<Impl>(queue,capacity)) {}
Scene::~Scene()=default;
core::Result<graph::NodeId> Scene::create(std::vector<runtime::PropertyDefinition> definitions,std::optional<graph::NodeId> parent) {
    if (!impl_->queue.is_current()) return core::fail(core::ErrorCode::wrong_executor,"Scene belongs to another execution domain");
    if (parent) if (auto valid=impl_->check(*parent); !valid) return std::unexpected(valid.error());
    // Build before changing topology, so an invalid definition leaves no node.
    std::unique_ptr<runtime::ObjectProperties> properties;
    try { properties=std::make_unique<runtime::ObjectProperties>(impl_->queue,std::move(definitions)); }
    catch (const std::invalid_argument&) { return core::fail(core::ErrorCode::invalid_argument,"Invalid scene properties",std::current_exception()); }
    auto id=impl_->graph.add(); if (!id) return std::unexpected(id.error());
    try {
        if (parent) {
            auto connected=impl_->graph.connect(*parent,*id);
            if (!connected) { (void)impl_->graph.remove(*id); return std::unexpected(connected.error()); }
        }
    } catch (...) { (void)impl_->graph.remove(*id); throw; }
    impl_->nodes[id->slot]={parent,std::move(properties)};
    return *id;
}
core::Result<void> Scene::reparent(graph::NodeId node,std::optional<graph::NodeId> parent) {
    if (auto valid=impl_->check(node); !valid) return valid;
    auto& previous=impl_->nodes[node.slot].parent;
    if (parent==previous) return {};
    if (parent) {
        auto connected=impl_->graph.connect(*parent,node); // validates cycle before unlinking
        if (!connected) return connected;
    }
    if (previous) (void)impl_->graph.disconnect(*previous,node);
    previous=parent; return {};
}
core::Result<void> Scene::erase(graph::NodeId subtree) {
    if (auto valid=impl_->check(subtree); !valid) return valid;
    std::vector<graph::NodeId> pending{subtree};
    for (std::size_t i=0;i<pending.size();++i) {
        auto children=impl_->graph.children(pending[i]);
        if (!children) return std::unexpected(children.error());
        pending.insert(pending.end(),children->begin(),children->end());
    }
    std::vector<std::unique_ptr<runtime::ObjectProperties>> retired;
    retired.reserve(pending.size());
    for (auto i=pending.rbegin();i!=pending.rend();++i) {
        retired.push_back(std::move(impl_->nodes[i->slot].properties));
        (void)impl_->graph.remove(*i);
        impl_->nodes[i->slot]={};
    }
    // User-owned property values can run destructors. Expose a fully detached
    // hierarchy before retiring them, so reentrant creation can reuse slots.
    return {};
}
core::Result<std::reference_wrapper<runtime::ObjectProperties>> Scene::properties(graph::NodeId node) {
    if (auto valid=impl_->check(node); !valid) return std::unexpected(valid.error());
    return std::ref(*impl_->nodes[node.slot].properties);
}
core::Result<std::optional<graph::NodeId>> Scene::parent(graph::NodeId node) const {
    if (auto valid=impl_->check(node); !valid) return std::unexpected(valid.error());
    return impl_->nodes[node.slot].parent;
}
} // namespace ii::scene
