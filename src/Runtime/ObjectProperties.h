#pragma once

#include "Query.h"
#include "Value.h"
#include <cstdint>
#include <optional>
#include <string>
#include <typeindex>
#include <variant>
#include <vector>

namespace ii::runtime {

using PropertyPath = std::vector<std::string>;
enum class PropertyVisibility { public_member, private_member };
enum class PropertyTyping { fixed, dynamic };
struct PropertyPolicy final {
    PropertyVisibility visibility = PropertyVisibility::public_member;
    bool queryable = true;
    bool designable = false;
    bool writable = false;
    PropertyTyping typing = PropertyTyping::fixed;
};

// Each instance owns its definitions. DynamicValue holds exactly the declared C++
// type; references/whole objects are included only when explicitly registered.
struct PropertyDefinition final {
    std::string name;
    DynamicValue value;
    PropertyPolicy policy;
    std::vector<PropertyDefinition> children;
    std::function<DynamicValue(const DynamicValue&)> method;
    std::function<bool(const DynamicValue&)> validate;
};
struct PropertySelection final {
    std::string name;
    bool include_value = true;
    std::vector<PropertySelection> children;
    std::optional<std::type_index> cast_to;
};
struct PropertySnapshot final {
    std::string name;
    DynamicValue value;
    std::vector<PropertySnapshot> children;
};
struct PropertyDescriptor final {
    PropertyPolicy policy;
    std::type_index value_type{typeid(void)};
    bool callable = false;
    bool validated = false;
    std::vector<std::string> children;
};

struct PropertyRead final { std::vector<PropertySelection> fields; };
struct PropertyWrite final { PropertyPath path; DynamicValue value; bool convert = false; };
struct PropertyInvoke final { PropertyPath path; DynamicValue argument; };
struct PropertyDescribe final { PropertyPath path; };
// Define beneath parent; replacing an existing subtree requires explicit opt-in.
struct PropertyDefine final { PropertyPath parent; PropertyDefinition definition; bool replace = false; };
struct PropertyErase final { PropertyPath path; };
using PropertyQuery = std::variant<PropertyRead, PropertyWrite, PropertyInvoke,
                                   PropertyDescribe, PropertyDefine, PropertyErase>;

enum class PropertyErrorCode {
    not_found, access_denied, not_writable, type_mismatch, invalid_definition,
    already_exists, validation_failed, not_callable, reentrant, handler_failed, wrong_executor, cast_failed
};
struct PropertyError final {
    PropertyErrorCode code;
    PropertyPath path;
    std::exception_ptr cause;
    std::optional<CastError> cast_error;
};
enum class PropertyChangeKind { defined, replaced, written, erased };
struct PropertyChange final {
    PropertyChangeKind kind;
    PropertyPath path;
    std::uint64_t revision;
};
struct PropertyResponse final {
    std::vector<PropertySnapshot> fields;
    DynamicValue returned;
    std::optional<PropertyDescriptor> descriptor;
    std::uint64_t revision = 0;
    // Notification failure is separate from the edit, which already committed.
    std::optional<QueryError> notification_error;
};
using PropertyResult = std::expected<PropertyResponse, PropertyError>;

// Compose this as a private member of an application object; share only queries().
// Bound instances use one SerialQueue; unbound instances require external serialization.
class ObjectProperties final {
public:
    using ChangeHandler = QueryPort<PropertyChange>::Handler;
    explicit ObjectProperties(std::vector<PropertyDefinition> definitions = {},
                              PropertyPolicy root_policy = {}, ChangeHandler on_change = {});
    ObjectProperties(SerialQueue& executor, std::vector<PropertyDefinition> definitions = {},
                     PropertyPolicy root_policy = {}, ChangeHandler on_change = {});
    ~ObjectProperties();
    ObjectProperties(const ObjectProperties&) = delete;
    ObjectProperties& operator=(const ObjectProperties&) = delete;
    ObjectProperties(ObjectProperties&&) = delete;
    ObjectProperties& operator=(ObjectProperties&&) = delete;

    // Owner authority bypasses policy, but never type checks or validation.
    [[nodiscard]] PropertyResult apply(const PropertyQuery& query);
    [[nodiscard]] const QueryPort<PropertyQuery, PropertyResult>& queries() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    // Inspect only in the owning execution domain, including reentrant methods.
    [[nodiscard]] bool is_executing() const noexcept;

private:
    struct Impl;
    std::optional<std::reference_wrapper<SerialQueue>> executor_;
    std::unique_ptr<Impl> impl_;
    QueryPort<PropertyQuery, PropertyResult> queries_;
};

} // namespace ii::runtime
