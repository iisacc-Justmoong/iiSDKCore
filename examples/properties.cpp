#include <iiSDKCore/Runtime/ObjectProperties.h>
#include <iostream>
#include <stdexcept>

using namespace ii::runtime;

// Static C++ structure and a runtime member tree compose without inheritance.
class Device final {
    static PropertyPolicy editable() {
        return {.queryable = true, .designable = true, .writable = true};
    }
    ObjectProperties properties_;
public:
    explicit Device(int level) : properties_({
        {.name = "audio", .policy = editable(), .children = {
            {.name = "output", .policy = editable(), .children = {
                {.name = "level", .value = level, .policy = editable(),
                 .validate = [](const std::any& value) {
                     const auto amount = std::any_cast<int>(value);
                     return amount >= 0 && amount <= 100;
                 }}}}}}}, editable()) {}

    const QueryPort<PropertyQuery, PropertyResult>& queries() const { return properties_.queries(); }
};

PropertyResponse query(Device& device, PropertyQuery request) {
    auto reply = device.queries().send(request);
    if (!reply || !*reply) throw std::runtime_error("property query failed");
    return std::move(**reply);
}

int main() {
    try {
        Device first(20), second(80);
        query(first, PropertyDefine{{"audio", "output"}, {
            .name = "gain", .value = 0.5,
            .policy = {.queryable = true, .designable = true, .writable = true}}});
        query(first, PropertyWrite{{"audio", "output", "gain"}, 0.75});
        query(first, PropertyDefine{{"audio", "output"}, {
            .name = "scale", .policy = {.queryable = true, .designable = true},
            .method = [](const std::any& input) -> std::any { return std::any_cast<double>(input) * 2.0; }}});
        const auto result = query(first, PropertyInvoke{{"audio", "output", "scale"}, 0.75});
        const PropertyRead selected{{{.name = "audio", .children = {
            {.name = "output", .children = {{.name = "gain"}}}}}}};
        const auto snapshot = query(first, selected);
        const double gain = std::any_cast<double>(snapshot.fields[0].children[0].children[0].value);
        const auto other = second.queries().send(selected);
        if (!other || *other || other->error().code != PropertyErrorCode::not_found) return 2;
        std::cout << "dynamic instance complete: gain=" << gain
                  << " scaled=" << std::any_cast<double>(result.returned)
                  << " other_instance_unchanged=1\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
