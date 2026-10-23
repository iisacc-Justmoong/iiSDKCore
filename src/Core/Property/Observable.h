#pragma once
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
struct ObservableLifetime final {
    std::atomic_bool active{true};
};
}
class ObservableSubscription final {
public:
    ObservableSubscription()=default;
    ~ObservableSubscription(){reset();}
    ObservableSubscription(const ObservableSubscription&)=delete;
    ObservableSubscription& operator=(const ObservableSubscription&)=delete;
    ObservableSubscription(ObservableSubscription&& other)noexcept:state_(std::move(other.state_)),owner_(std::move(other.owner_)){}
    ObservableSubscription& operator=(ObservableSubscription&& other)noexcept{
        if(this!=&other){reset();state_=std::move(other.state_);owner_=std::move(other.owner_);}return *this;
    }
    void reset()noexcept{if(state_)state_->active=false;state_.reset();owner_.reset();}
    [[nodiscard]] bool active()const noexcept{return state_&&state_->active.load();}
private:
    template<class>friend class Observable;
    ObservableSubscription(std::shared_ptr<detail::ObservableLifetime> state,std::shared_ptr<void> owner):state_(std::move(state)),owner_(std::move(owner)){}
    std::shared_ptr<detail::ObservableLifetime> state_;
    std::shared_ptr<void> owner_;
};

// Typed, owner-thread properties for compiled/native consumers. Plain C++ writes
// are deliberately not intercepted. Mutate through set(); observe with a lease.
template<class T> class Observable final {
    static_assert(std::copy_constructible<T>&&std::equality_comparable<T>&&std::is_nothrow_move_assignable_v<T>);
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
    [[nodiscard]] const T& get()const{check();return state_->value;}
    [[nodiscard]] std::uint64_t revision()const{check();return state_->revision;}
    bool set(T value){
        check();auto state=state_;if(state->value==value)return false;
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
    [[nodiscard]] ObservableSubscription observe(std::function<void(const T&,std::uint64_t)> callback,bool initial=false){
        check();if(!callback)throw std::invalid_argument("Observable requires a callback");auto state=state_;
        for(auto it=state->observers.begin();it!=state->observers.end();)if(it->second.expired())it=state->observers.erase(it);else ++it;
        if(state->observers.size()>=maximum_observers||state->next==UINT64_MAX)throw std::length_error("Observable observer capacity exhausted");
        auto slot=std::make_shared<Slot>();slot->callback=std::move(callback);state->observers.emplace(++state->next,slot);
        ObservableSubscription lease(slot->lifetime,slot);if(initial)slot->callback(state->value,state->revision);return lease;
    }
private:
    void check()const{
        if(!state_||state_->closed)throw std::logic_error("Observable is closed");
        if(state_->owner!=std::this_thread::get_id())throw std::logic_error("Observable requires its owner thread");
    }
    void close()noexcept{
        if(!state_)return;state_->closed=true;
        for(auto& [id,weak]:state_->observers)if(auto slot=weak.lock())slot->lifetime->active=false;
    }
    std::shared_ptr<State> state_;
};
}
