#include "Bindings.h"
#include <map>

namespace ii::scripting {
struct Bindings::State {
    runtime::SerialQueue& queue;
    std::map<std::string,std::shared_ptr<Function>,std::less<>> functions;
    explicit State(runtime::SerialQueue& domain) : queue(domain) {}
    CallResult call(const Call& request) {
        auto found=functions.find(request.name);
        if (found==functions.end()) return core::fail(core::ErrorCode::not_found,"Script binding not found");
        auto function=found->second; // retain self-replaced/removed native callables
        try { CallResponse response; response.value=(*function)(request.argument); return response; }
        catch (...) { return core::fail(core::ErrorCode::failed,"Native script binding failed",std::current_exception()); }
    }
};
Bindings::Bindings(runtime::SerialQueue& queue,std::vector<Binding> definitions)
    : state_(std::make_shared<State>(queue)),queries_(queue,[state=state_](const Call& call) { return state->call(call); }) {
    for (auto& definition:definitions) {
        if (definition.name.empty() || !definition.function || state_->functions.contains(definition.name))
            throw std::invalid_argument("Invalid or duplicate script binding");
        state_->functions.emplace(std::move(definition.name),std::make_shared<Function>(std::move(definition.function)));
    }
}
core::Result<void> Bindings::define(std::string name,Function function,bool replace) {
    if (!state_->queue.is_current()) return core::fail(core::ErrorCode::wrong_executor,"Bindings belong to another execution domain");
    if (name.empty() || !function) return core::fail(core::ErrorCode::invalid_argument,"Invalid script binding");
    if (!replace && state_->functions.contains(name)) return core::fail(core::ErrorCode::already_exists,"Binding already exists");
    auto prepared=std::make_shared<Function>(std::move(function));
    auto found=state_->functions.find(name);
    if (found==state_->functions.end()) state_->functions.emplace(std::move(name),std::move(prepared));
    else found->second.swap(prepared); // retire replaced captures after the map is stable
    return {};
}
core::Result<void> Bindings::remove(const std::string& name) {
    if (!state_->queue.is_current()) return core::fail(core::ErrorCode::wrong_executor,"Bindings belong to another execution domain");
    auto retired=state_->functions.extract(name);
    if (retired.empty()) return core::fail(core::ErrorCode::not_found,"Script binding not found");
    return {};
}
} // namespace ii::scripting
