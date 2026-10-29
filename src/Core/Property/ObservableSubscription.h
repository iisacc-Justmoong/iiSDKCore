#pragma once
#include <atomic>
#include <memory>
#include <utility>

namespace ii::core {
namespace detail {
struct ObservableLifetime final {std::atomic_bool active{true};};
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
}
