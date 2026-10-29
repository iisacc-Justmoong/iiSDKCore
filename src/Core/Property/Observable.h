#pragma once
#include "ObservableReadScope.h"
#include <atomic>
#include <concepts>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace ii::core {
namespace detail {
template<class T>struct ObservableValueOwner final {static std::shared_ptr<const void> get(const T&){return {};}};
template<class T>struct ObservableValueOwner<std::shared_ptr<T>> final {static std::shared_ptr<const void> get(const std::shared_ptr<T>& value){return std::shared_ptr<const void>(value,nullptr);}};
}

// Typed, owner-thread properties for compiled/native consumers. Plain C++ writes
// are deliberately not intercepted. Mutate through set(); observe with a lease.
template<class T> class Observable final {
    static_assert(std::copy_constructible<T>&&std::is_nothrow_move_assignable_v<T>);
    struct Slot final{std::shared_ptr<detail::ObservableLifetime> lifetime=std::make_shared<detail::ObservableLifetime>();std::function<void(const T&,std::uint64_t)> callback;};
    struct Change final{T value;std::uint64_t revision;};
    struct State final{
        explicit State(T initial):value(std::move(initial)),owner(std::this_thread::get_id()){}
        T value;std::thread::id owner;std::uint64_t revision=0,next=0;
        std::map<std::uint64_t,std::weak_ptr<Slot>> observers;
        std::deque<Change> pending;
        std::size_t delivered=0;bool dispatching=false,closed=false;
    };
public:
    static constexpr std::size_t maximum_notifications=256,maximum_observers=1024;
    explicit Observable(T initial={}):state_(std::make_shared<State>(std::move(initial))){}
    ~Observable(){close();}
    Observable(const Observable&)=delete;
    Observable& operator=(const Observable&)=delete;
    Observable(Observable&& other)noexcept:state_(std::move(other.state_)){}
    Observable& operator=(Observable&& other)noexcept{if(this!=&other){close();state_=std::move(other.state_);}return *this;}
    [[nodiscard]] const T& get()const{
        check();if(!ObservableReadScope::active())return state_->value;
        auto state=state_;ObservableReadScope::record(read_dependency());check_state(state);return state->value;
    }
    [[nodiscard]] std::uint64_t revision()const{check();return state_->revision;}
    [[nodiscard]] ObservableRead read_dependency()const{
        check();auto state=state_;
        return {state.get(),state->revision,[state](std::function<void()> changed){if(!changed)throw std::invalid_argument("Observable read requires a callback");return observe_state(state,[changed=std::move(changed)](const T&,std::uint64_t){changed();},false);},
            [state]{check_state(state);return state->revision;},std::shared_ptr<const void>(state,nullptr),detail::ObservableValueOwner<T>::get(state->value)};
    }
    bool set(T value){
        check();auto state=state_;
        if constexpr(std::equality_comparable<T>)if(state->value==value)return false;
        if(state->revision==UINT64_MAX)throw std::length_error("Observable revision exhausted");
        if(state->dispatching&&state->delivered+state->pending.size()>=maximum_notifications)
            throw std::length_error("Observable update cycle exceeded its admission bound");
        state->pending.push_back({value,state->revision+1}); // Allocation precedes the committed mutation.
        state->value=std::move(value);++state->revision;
        if(state->dispatching)return true;
        state->dispatching=true;state->delivered=0;
        struct Reset final{std::shared_ptr<State> state;~Reset(){state->dispatching=false;state->pending.clear();}}reset{state};
        std::exception_ptr failure;
        while(!state->pending.empty()&&!state->closed){
            auto change=std::move(state->pending.front());state->pending.pop_front();++state->delivered;
            std::vector<std::shared_ptr<Slot>> delivery;
            for(auto it=state->observers.begin();it!=state->observers.end();){
                auto slot=it->second.lock();if(!slot||!slot->lifetime->active){it=state->observers.erase(it);continue;}
                delivery.push_back(std::move(slot));++it;
            }
            for(const auto& slot:delivery){if(state->closed)break;if(!slot->lifetime->active)continue;
                try{slot->callback(change.value,change.revision);}catch(...){if(!failure)failure=std::current_exception();}}
        }
        if(failure)std::rethrow_exception(failure);return true;
    }
    [[nodiscard]] ObservableSubscription observe(std::function<void(const T&,std::uint64_t)> callback,bool initial=false)const{
        check();return observe_state(state_,std::move(callback),initial);
    }
private:
    static ObservableSubscription observe_state(const std::shared_ptr<State>& state,std::function<void(const T&,std::uint64_t)> callback,bool initial){
        check_state(state);if(!callback)throw std::invalid_argument("Observable requires a callback");
        for(auto it=state->observers.begin();it!=state->observers.end();)if(it->second.expired())it=state->observers.erase(it);else ++it;
        if(state->observers.size()>=maximum_observers||state->next==UINT64_MAX)throw std::length_error("Observable observer capacity exhausted");
        auto slot=std::make_shared<Slot>();slot->callback=std::move(callback);state->observers.emplace(++state->next,slot);
        ObservableSubscription lease(slot->lifetime,slot);if(initial)slot->callback(state->value,state->revision);return lease;
    }
    static void check_state(const std::shared_ptr<State>& state){
        if(!state||state->closed)throw std::logic_error("Observable is closed");
        if(state->owner!=std::this_thread::get_id())throw std::logic_error("Observable requires its owner thread");
    }
    void check()const{check_state(state_);}
    void close()noexcept{
        if(!state_)return;state_->closed=true;
        for(auto& [id,weak]:state_->observers)if(auto slot=weak.lock())slot->lifetime->active=false;
    }
    std::shared_ptr<State> state_;
};
}
