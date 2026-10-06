#include "ObjectProperties.h"

#include <map>
#include <atomic>

namespace ii::runtime {
namespace {

struct Node {
    std::any value;
    PropertyPolicy policy;
    std::map<std::string, std::unique_ptr<Node>, std::less<>> children;
    std::shared_ptr<std::function<std::any(const std::any&)>> method;
    std::function<bool(const std::any&)> validate;
};
using Failure = std::unexpected<PropertyError>;
Failure failure(PropertyErrorCode code, const PropertyPath& path, std::exception_ptr cause = {}) {
    return Failure(PropertyError{code, path, cause, {}});
}
PropertyPath child_path(const PropertyPath& parent, const std::string& name) {
    auto path = parent;
    path.push_back(name);
    return path;
}
bool allowed(const Node& node, bool design) {
    return node.policy.visibility == PropertyVisibility::public_member &&
           (design ? node.policy.designable : node.policy.queryable);
}
std::expected<void, PropertyError> validate(const Node& node, const std::any& value,
                                          const PropertyPath& path) {
    if (!node.validate) return {};
    try {
        if (!node.validate(value)) return failure(PropertyErrorCode::validation_failed, path);
    } catch (...) {
        return failure(PropertyErrorCode::handler_failed, path, std::current_exception());
    }
    return {};
}
std::expected<std::unique_ptr<Node>, PropertyError> build(const PropertyDefinition& definition,
                                                       const PropertyPath& path) {
    if (definition.name.empty() || (definition.validate && !definition.value.has_value()))
        return failure(PropertyErrorCode::invalid_definition, path);
    auto node = std::make_unique<Node>();
    node->value = definition.value;
    node->policy = definition.policy;
    if (definition.method)
        node->method = std::make_shared<std::function<std::any(const std::any&)>>(definition.method);
    node->validate = definition.validate;
    if (auto result = validate(*node, node->value, path); !result) return std::unexpected(result.error());
    for (const auto& child : definition.children) {
        auto nested_path = child_path(path, child.name);
        if (node->children.contains(child.name)) return failure(PropertyErrorCode::invalid_definition, nested_path);
        auto nested = build(child, nested_path);
        if (!nested) return std::unexpected(nested.error());
        node->children.emplace(child.name, std::move(*nested));
    }
    return node;
}
std::expected<void, PropertyError> editable_subtree(const Node& node, const PropertyPath& path) {
    // Replacing a parent must not bypass policy on a protected descendant.
    if (!allowed(node, true)) return failure(PropertyErrorCode::access_denied, path);
    for (const auto& [name, child] : node.children) {
        if (auto result = editable_subtree(*child, child_path(path, name)); !result) return result;
    }
    return {};
}

struct EditGuard {
    bool& busy;
    bool active = true;
    explicit EditGuard(bool& flag) : busy(flag) { busy = true; }
    void release() noexcept { if (active) { busy = false; active = false; } }
    ~EditGuard() { release(); }
};

} // namespace

struct ObjectProperties::Impl {
    Node root;
    std::atomic<std::uint64_t> revision{0};
    bool busy = false;
    std::size_t executing = 0;
    std::optional<QueryPort<PropertyChange>> changed;

    Impl(const std::vector<PropertyDefinition>& definitions, PropertyPolicy policy, ChangeHandler handler) {
        root.policy = policy;
        for (const auto& definition : definitions) {
            if (root.children.contains(definition.name))
                throw std::invalid_argument("ObjectProperties: duplicate root member");
            auto node = build(definition, {definition.name});
            if (!node) throw std::invalid_argument("ObjectProperties: invalid initial definition or value");
            root.children.emplace(definition.name, std::move(*node));
        }
        if (handler) changed.emplace(std::move(handler));
    }

    std::expected<std::reference_wrapper<Node>, PropertyError> find(const PropertyPath& path,
                                                                 bool owner, bool design) {
        auto* node = &root;
        if (!owner && !allowed(*node, design)) return failure(PropertyErrorCode::access_denied, {});
        PropertyPath traversed;
        for (const auto& name : path) {
            traversed.push_back(name);
            const auto it = node->children.find(name);
            if (it == node->children.end()) return failure(PropertyErrorCode::not_found, traversed);
            node = it->second.get();
            if (!owner && !allowed(*node, design)) return failure(PropertyErrorCode::access_denied, traversed);
        }
        return std::ref(*node);
    }

    std::expected<std::vector<PropertySnapshot>, PropertyError> select(
        const Node& node, const std::vector<PropertySelection>& selections, const PropertyPath& path, bool owner) {
        if (!owner && !allowed(node, false)) return failure(PropertyErrorCode::access_denied, path);
        std::vector<PropertySnapshot> fields;
        fields.reserve(selections.size());
        for (const auto& selection : selections) {
            auto nested_path = child_path(path, selection.name);
            const auto it = node.children.find(selection.name);
            if (it == node.children.end()) return failure(PropertyErrorCode::not_found, nested_path);
            auto children = select(*it->second, selection.children, nested_path, owner);
            if (!children) return std::unexpected(children.error());
            PropertySnapshot snapshot{selection.name, {}, std::move(*children)};
            if (selection.include_value) {
                if (selection.cast_to) {
                    auto converted = cast_value(it->second->value, *selection.cast_to);
                    if (!converted) return Failure(PropertyError{PropertyErrorCode::cast_failed, nested_path, {}, converted.error()});
                    snapshot.value = std::move(*converted);
                } else snapshot.value = it->second->value;
            }
            fields.push_back(std::move(snapshot));
        }
        return fields;
    }

    PropertyResult finish(PropertyChange change, EditGuard& guard) {
        change.revision = ++revision;
        PropertyResponse response;
        response.revision = change.revision;
        guard.release();
        // Queries to an optional notification receiver run only after commit.
        if (changed) {
            auto reply = changed->send(change);
            if (!reply) response.notification_error = reply.error();
        }
        return response;
    }

    PropertyResult run(const PropertyRead& query, bool owner, EditGuard&) {
        auto fields = select(root, query.fields, {}, owner);
        if (!fields) return std::unexpected(fields.error());
        PropertyResponse response;
        response.fields = std::move(*fields);
        response.revision = revision;
        return response;
    }

    PropertyResult run(const PropertyDescribe& query, bool owner, EditGuard&) {
        auto found = find(query.path, owner, true);
        if (!found) return std::unexpected(found.error());
        const auto& node = found->get();
        PropertyDescriptor descriptor{node.policy, node.value.type(), bool(node.method), bool(node.validate), {}};
        for (const auto& [name, child] : node.children)
            if (owner || allowed(*child, true)) descriptor.children.push_back(name);
        PropertyResponse response;
        response.descriptor = std::move(descriptor);
        response.revision = revision;
        return response;
    }

    PropertyResult run(const PropertyWrite& query, bool owner, EditGuard& guard) {
        auto found = find(query.path, owner, false);
        if (!found) return std::unexpected(found.error());
        auto& node = found->get();
        if (!owner && !node.policy.writable) return failure(PropertyErrorCode::not_writable, query.path);
        if (!node.value.has_value() || !query.value.has_value())
            return failure(PropertyErrorCode::type_mismatch, query.path);
        auto prepared = query.value;
        if (query.convert) {
            auto converted = cast_value(query.value, node.value.type());
            if (!converted) return Failure(PropertyError{PropertyErrorCode::cast_failed, query.path, {}, converted.error()});
            prepared = std::move(*converted);
        } else if (node.policy.typing == PropertyTyping::fixed && node.value.type() != query.value.type())
            return failure(PropertyErrorCode::type_mismatch, query.path);
        if (auto result = validate(node, prepared, query.path); !result) return std::unexpected(result.error());
        PropertyChange change{PropertyChangeKind::written, query.path, 0};
        node.value.swap(prepared);
        // Destroy replaced values while guarded, before the post-commit query.
        prepared.reset();
        return finish(std::move(change), guard);
    }

    PropertyResult run(const PropertyInvoke& query, bool owner, EditGuard& guard) {
        auto found = find(query.path, owner, false);
        if (!found) return std::unexpected(found.error());
        if (!found->get().method) return failure(PropertyErrorCode::not_callable, query.path);
        // Retain this invocation even if its method is replaced/removed by itself.
        auto method = found->get().method;
        guard.release();
        try {
            PropertyResponse response;
            response.returned = (*method)(query.argument);
            response.revision = revision;
            return response;
        } catch (...) {
            return failure(PropertyErrorCode::handler_failed, query.path, std::current_exception());
        }
    }

    PropertyResult run(const PropertyDefine& query, bool owner, EditGuard& guard) {
        auto found = find(query.parent, owner, true);
        if (!found) return std::unexpected(found.error());
        auto& children = found->get().children;
        const auto existing = children.find(query.definition.name);
        auto path = child_path(query.parent, query.definition.name);
        if (existing != children.end()) {
            if (!owner) {
                auto permitted = editable_subtree(*existing->second, path);
                if (!permitted) return failure(PropertyErrorCode::access_denied, path);
            }
            if (!query.replace) return failure(PropertyErrorCode::already_exists, path);
        }
        auto prepared = build(query.definition, path);
        if (!prepared) return std::unexpected(prepared.error());
        PropertyChange change{existing == children.end() ? PropertyChangeKind::defined : PropertyChangeKind::replaced,
                              std::move(path), 0};
        if (existing == children.end()) children.emplace(query.definition.name, std::move(*prepared));
        else {
            existing->second.swap(*prepared);
            prepared->reset();
        }
        return finish(std::move(change), guard);
    }

    PropertyResult run(const PropertyErase& query, bool owner, EditGuard& guard) {
        if (query.path.empty()) return failure(PropertyErrorCode::invalid_definition, query.path);
        auto found = find(query.path, owner, true);
        if (!found) return std::unexpected(found.error());
        if (!owner) {
            auto permitted = editable_subtree(found->get(), query.path);
            if (!permitted) return failure(PropertyErrorCode::access_denied, query.path);
        }
        auto parent = query.path;
        parent.pop_back();
        auto parent_node = find(parent, owner, true);
        PropertyChange change{PropertyChangeKind::erased, query.path, 0};
        parent_node->get().children.erase(query.path.back());
        return finish(std::move(change), guard);
    }

    PropertyResult execute(const PropertyQuery& query, bool owner) {
        ++executing;
        struct Finish {
            std::size_t& count;
            ~Finish() { --count; }
        } finish{executing};
        if (busy) return failure(PropertyErrorCode::reentrant, {});
        EditGuard guard(busy);
        return std::visit([&](const auto& operation) { return run(operation, owner, guard); }, query);
    }
};

ObjectProperties::ObjectProperties(std::vector<PropertyDefinition> definitions, PropertyPolicy policy,
                                   ChangeHandler on_change)
    : impl_(std::make_unique<Impl>(definitions, policy, std::move(on_change))),
      queries_([this](const PropertyQuery& query) { return impl_->execute(query, false); }) {}
ObjectProperties::~ObjectProperties() {
    if (impl_->executing) std::terminate();
    const auto result=queries_.close();
    if (!result) std::terminate();
}
ObjectProperties::ObjectProperties(SerialQueue& executor, std::vector<PropertyDefinition> definitions,
                                   PropertyPolicy policy, ChangeHandler on_change)
    : executor_(std::ref(executor)), impl_(std::make_unique<Impl>(definitions, policy, std::move(on_change))),
      queries_(executor, [this](const PropertyQuery& query) { return impl_->execute(query, false); }) {}
PropertyResult ObjectProperties::apply(const PropertyQuery& query) {
    if (executor_ && !executor_->get().is_current())
        return failure(PropertyErrorCode::wrong_executor, {});
    return impl_->execute(query, true);
}
const QueryPort<PropertyQuery, PropertyResult>& ObjectProperties::queries() const noexcept { return queries_; }
std::uint64_t ObjectProperties::revision() const noexcept { return impl_->revision; }
bool ObjectProperties::is_executing() const noexcept { return impl_->executing || queries_.is_current(); }

} // namespace ii::runtime
