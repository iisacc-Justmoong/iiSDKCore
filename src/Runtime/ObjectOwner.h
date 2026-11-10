#pragma once

#include "Thread.h"

#include <concepts>
#include <expected>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace ii::runtime {

enum class ObjectError { closed, invalid_object };

namespace detail {
struct ObjectOwnerState;
struct OwnedEntry;
using ObjectVisitor = void (*)(void*, void*);
using ObjectDestroy = void (*)(void*) noexcept;
using OwnedValue = std::unique_ptr<void, ObjectDestroy>;
std::expected<std::shared_ptr<OwnedEntry>, ObjectError>
own_object(const std::shared_ptr<ObjectOwnerState>&, OwnedValue);
SubmitError access_object(const std::weak_ptr<ObjectOwnerState>&,
                          const std::weak_ptr<OwnedEntry>&, void*, ObjectVisitor);
ThreadResult erase_object(const std::shared_ptr<ObjectOwnerState>&,
                          const std::weak_ptr<ObjectOwnerState>&,
                          const std::weak_ptr<OwnedEntry>&);
bool object_expired(const std::weak_ptr<ObjectOwnerState>&, const std::weak_ptr<OwnedEntry>&);
}

// A non-owning identity. A reference to T is valid only inside with(); a
// handle never extends the lifetime of the object or its owner.
template<class T>
class ObjectRef final {
public:
    ObjectRef() = default;
    template<class F> requires std::invocable<F&, T&>
    [[nodiscard]] SubmitError with(F&& function) const {
        auto visit = [&](void* value) { std::invoke(function, *static_cast<T*>(value)); };
        return detail::access_object(owner_, entry_, &visit, [](void* context, void* object) {
            (*static_cast<decltype(visit)*>(context))(object);
        });
    }
    [[nodiscard]] bool expired() const { return detail::object_expired(owner_, entry_); }
private:
    friend class ObjectOwner;
    ObjectRef(const std::shared_ptr<detail::ObjectOwnerState>& owner,
              const std::shared_ptr<detail::OwnedEntry>& entry) : owner_(owner), entry_(entry) {}
    std::weak_ptr<detail::ObjectOwnerState> owner_;
    std::weak_ptr<detail::OwnedEntry> entry_;
};

// Owns objects exclusively. Operations on an object may overlap; the host is
// responsible for synchronizing the object's own mutable state.
class ObjectOwner final {
public:
    ObjectOwner();
    ~ObjectOwner();
    ObjectOwner(const ObjectOwner&) = delete;
    ObjectOwner& operator=(const ObjectOwner&) = delete;
    ObjectOwner(ObjectOwner&&) = delete;
    ObjectOwner& operator=(ObjectOwner&&) = delete;

    template<class T, class... Args>
        requires std::is_object_v<T> && (!std::is_array_v<T>) && (!std::is_const_v<T>) && std::constructible_from<T, Args...>
    [[nodiscard]] std::expected<ObjectRef<T>, ObjectError> emplace(Args&&... args) const {
        static_assert(std::is_nothrow_destructible_v<T>, "owned object destructors must be noexcept");
        if (closed()) return std::unexpected(ObjectError::closed);
        return adopt(std::make_unique<T>(std::forward<Args>(args)...));
    }
    // Consumes object even when admission loses a race with close.
    template<class T> requires std::is_object_v<T> && (!std::is_array_v<T>) && (!std::is_const_v<T>)
    [[nodiscard]] std::expected<ObjectRef<T>, ObjectError> adopt(std::unique_ptr<T> object) const {
        static_assert(std::is_nothrow_destructible_v<T>, "owned object destructors must be noexcept");
        if (!object) return std::unexpected(ObjectError::invalid_object);
        detail::OwnedValue erased(object.release(),[](void* value) noexcept { delete static_cast<T*>(value); });
        auto result=detail::own_object(state_,std::move(erased));
        if (!result) return std::unexpected(result.error());
        return ObjectRef<T>(state_,*result);
    }
    template<class T>
    [[nodiscard]] ThreadResult erase(const ObjectRef<T>& object) const {
        return detail::erase_object(state_,object.owner_,object.entry_);
    }

    void request_close() const;
    [[nodiscard]] ThreadResult close() const;
    [[nodiscard]] ThreadResult try_close() const;
    [[nodiscard]] bool closed() const;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] std::size_t size() const;
private:
    std::shared_ptr<detail::ObjectOwnerState> state_;
};

} // namespace ii::runtime
