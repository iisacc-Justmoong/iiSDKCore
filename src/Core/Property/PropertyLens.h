#pragma once
#include "../../Runtime/ObjectProperties.h"

namespace ii::core {
// Immutable reusable selection path; policy and execution checks stay at receiver.
class PropertyLens final {
public:
    explicit PropertyLens(runtime::PropertyPath path) : path_(std::move(path)) {
        if (path_.empty()) throw std::invalid_argument("PropertyLens requires a value path");
        for (const auto& part:path_) if (part.empty()) throw std::invalid_argument("Empty property path component");
    }
    [[nodiscard]] runtime::PropertyRead read(std::optional<std::type_index> cast={}) const {
        runtime::PropertySelection selected{.name=path_.back(),.cast_to=cast};
        for (std::size_t i=path_.size()-1;i>0;--i)
            selected=runtime::PropertySelection{.name=path_[i-1],.include_value=false,.children={std::move(selected)}};
        return {{std::move(selected)}};
    }
    [[nodiscard]] runtime::PropertyWrite write(runtime::DynamicValue value, bool convert=false) const { return {path_,std::move(value),convert}; }
    [[nodiscard]] runtime::PropertyInvoke invoke(runtime::DynamicValue argument={}) const { return {path_,std::move(argument)}; }
    [[nodiscard]] runtime::PropertyDescribe describe() const { return {path_}; }
private:
    runtime::PropertyPath path_;
};
} // namespace ii::core
