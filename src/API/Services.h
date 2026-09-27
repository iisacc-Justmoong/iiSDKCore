#pragma once
#include "../Core/Result/Result.h"
#include <any>
#include <functional>
#include <map>
#include <mutex>
#include <typeindex>

namespace ii::api {
// Explicit per-application API surface. Registrations borrow stable instances.
class Services final {
public:
    template<class T> [[nodiscard]] core::Result<void> expose(T& instance) {
        std::lock_guard lock(mutex_);
        if (!services_.emplace(std::type_index(typeid(std::reference_wrapper<T>)),std::ref(instance)).second)
            return core::fail(core::ErrorCode::already_exists,"Service type is already exposed");
        return {};
    }
    template<class T> [[nodiscard]] core::Result<std::reference_wrapper<T>> find() const {
        std::lock_guard lock(mutex_);
        auto found=services_.find(typeid(std::reference_wrapper<T>));
        if (found==services_.end()) return core::fail(core::ErrorCode::not_found,"Service type is not exposed");
        return std::any_cast<std::reference_wrapper<T>>(found->second);
    }
    template<class T> bool remove() { std::lock_guard lock(mutex_); return services_.erase(typeid(std::reference_wrapper<T>))!=0; }
private:
    mutable std::mutex mutex_;
    std::map<std::type_index,std::any> services_;
};
} // namespace ii::api
