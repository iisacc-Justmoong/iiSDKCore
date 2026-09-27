#pragma once
#include "../Core/Result/Result.h"
#include "../Runtime/Query.h"
#include "../Runtime/Value.h"
#include <vector>

namespace ii::scripting {
struct Call { std::string name; runtime::DynamicValue argument; };
struct CallResponse {
    runtime::DynamicValue value;
    // A non-aggregate prevents implicit aggregate construction through std::any
    // during nested std::expected constraint checks on older libc++ versions.
    CallResponse() = default;
};
using CallResult=core::Result<CallResponse>;
using Function=std::function<runtime::DynamicValue(const runtime::DynamicValue&)>;
struct Binding { std::string name; Function function; };
// Native callable boundary for an embedding host. No language parser or VM.
class Bindings final {
public:
    Bindings(runtime::SerialQueue& queue,std::vector<Binding> definitions={});
    [[nodiscard]] core::Result<void> define(std::string name,Function function,bool replace=false);
    [[nodiscard]] core::Result<void> remove(const std::string& name);
    [[nodiscard]] const runtime::QueryPort<Call,CallResult>& queries() const noexcept { return queries_; }
private:
    struct State;
    std::shared_ptr<State> state_;
    runtime::QueryPort<Call,CallResult> queries_;
};
} // namespace ii::scripting
