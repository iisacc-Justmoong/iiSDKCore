#pragma once
#include "ObservableRead.h"
#include "ObservationExport.h"
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace ii::core {
// A synchronous lexical collector, with no registry or notification bus.
// Only the innermost call receives reads; recording itself is not observed.
class ObservableReadScope final {
public:
    static constexpr std::size_t maximum_depth=64;
    template<class Collector,class Function>static decltype(auto) collect(Collector&& collector,Function&& function){
        auto receiver=[&](ObservableRead read){std::invoke(collector,std::move(read));};
        ObservableReadScope scope(&receiver,[](void* value,ObservableRead read){(*static_cast<decltype(receiver)*>(value))(std::move(read));});
        return std::invoke(std::forward<Function>(function));
    }
    template<class Function>static decltype(auto) pause(Function&& function){
        ObservableReadScope scope(nullptr,nullptr);return std::invoke(std::forward<Function>(function));
    }
    [[nodiscard]] IISDKCORE_OBSERVATION_API static bool active()noexcept;
    IISDKCORE_OBSERVATION_API static void record(ObservableRead read);
private:
    using Delivery=void(*)(void*,ObservableRead);
    IISDKCORE_OBSERVATION_API ObservableReadScope(void* receiver,Delivery deliver);
    IISDKCORE_OBSERVATION_API ~ObservableReadScope();
    ObservableReadScope(const ObservableReadScope&)=delete;
    ObservableReadScope& operator=(const ObservableReadScope&)=delete;
    void* receiver_;Delivery deliver_;ObservableReadScope* previous_;
};
}
