#pragma once
#include "ObservableSubscription.h"
#include <cstdint>
#include <functional>

namespace ii::core {
// A property read can outlive its lexical property object without storing a raw
// object pointer. Revision and subscription still reject a closed owner.
struct ObservableRead final {
    const void* identity=nullptr;
    std::uint64_t revision=0;
    std::function<ObservableSubscription(std::function<void()>)> subscribe;
    std::function<std::uint64_t()> current_revision;
    std::shared_ptr<const void> owner,value_owner;
};
}
