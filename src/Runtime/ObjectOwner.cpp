#include "ObjectOwner.h"
#include "TaskScope.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace ii::runtime {
namespace detail {
namespace {
ThreadResult failure(ThreadErrorCode code) { return std::unexpected(ThreadError{code,{}}); }
}
struct OwnedEntry {
    OwnedValue object;
    bool removing=false; // protected by ObjectOwnerState::mutex
    TaskScope scope;
    explicit OwnedEntry(OwnedValue value)
        : object(std::move(value)), scope([this]{object.reset();}) {}
};
struct ObjectOwnerState {
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<OwnedEntry>> entries;
    // Scope cleanup is last-declared so it runs before the registry disappears.
    TaskScope scope;
    ObjectOwnerState() : scope([this]{ release_all(); }) {}
    void release_all() {
        for (;;) {
            std::shared_ptr<OwnedEntry> entry;
            {
                std::lock_guard lock(mutex);
                if (entries.empty()) break;
                entry=entries.back();
            }
            // All access to entries runs inside the already-closed owner scope.
            const auto result=entry->scope.close();
            if (!result) std::terminate();
            {
                std::lock_guard lock(mutex);
                entries.pop_back();
            }
        }
    }
};

std::expected<std::shared_ptr<OwnedEntry>, ObjectError>
own_object(const std::shared_ptr<ObjectOwnerState>& owner, OwnedValue value) {
    std::expected<std::shared_ptr<OwnedEntry>, ObjectError> result=std::unexpected(ObjectError::closed);
    const auto admitted=owner->scope.run([&]{
        auto entry=std::make_shared<OwnedEntry>(std::move(value));
        {
            std::lock_guard lock(owner->mutex);
            owner->entries.push_back(entry);
        }
        result=std::move(entry);
    });
    if (admitted!=SubmitError::none) return std::unexpected(ObjectError::closed);
    return result;
}

SubmitError access_object(const std::weak_ptr<ObjectOwnerState>& owner_weak,
                          const std::weak_ptr<OwnedEntry>& entry_weak,
                          void* context, ObjectVisitor visitor) {
    auto owner=owner_weak.lock(); auto entry=entry_weak.lock();
    if (!owner || !entry || !visitor) return SubmitError::stopped;
    auto inner=SubmitError::stopped;
    const auto outer=owner->scope.run([&]{
        inner=entry->scope.run([&]{ visitor(context,entry->object.get()); });
    });
    return outer==SubmitError::none ? inner : outer;
}

ThreadResult erase_object(const std::shared_ptr<ObjectOwnerState>& owner,
                          const std::weak_ptr<ObjectOwnerState>& owner_weak,
                          const std::weak_ptr<OwnedEntry>& entry_weak) {
    auto claimed_owner=owner_weak.lock(); auto entry=entry_weak.lock();
    if (!entry || claimed_owner!=owner) return failure(ThreadErrorCode::invalid_work);
    if (entry->scope.is_current()) return failure(ThreadErrorCode::self_join);
    ThreadResult result=failure(ThreadErrorCode::invalid_work);
    const auto admitted=owner->scope.run([&]{
        {
            std::lock_guard lock(owner->mutex);
            if (std::find(owner->entries.begin(),owner->entries.end(),entry)==owner->entries.end()) return;
            if (entry->removing) { result=failure(ThreadErrorCode::busy); return; }
            entry->removing=true;
        }
        result=entry->scope.close();
        {
            std::lock_guard lock(owner->mutex);
            if (result) {
                const auto found=std::find(owner->entries.begin(),owner->entries.end(),entry);
                if (found!=owner->entries.end()) owner->entries.erase(found);
            } else entry->removing=false;
        }
    });
    if (admitted!=SubmitError::none) return failure(ThreadErrorCode::invalid_work);
    return result;
}

bool object_expired(const std::weak_ptr<ObjectOwnerState>& owner_weak,
                    const std::weak_ptr<OwnedEntry>& entry_weak) {
    auto owner=owner_weak.lock(); auto entry=entry_weak.lock();
    return !owner || !entry || owner->scope.closed() || entry->scope.closed();
}
} // namespace detail

ObjectOwner::ObjectOwner() : state_(std::make_shared<detail::ObjectOwnerState>()) {}
ObjectOwner::~ObjectOwner() {
    const auto result=close();
    if (!result && result.error().code!=ThreadErrorCode::work_failed) std::terminate();
}
void ObjectOwner::request_close() const { state_->scope.request_close(); }
ThreadResult ObjectOwner::close() const { return state_->scope.close(); }
ThreadResult ObjectOwner::try_close() const { return state_->scope.try_close(); }
bool ObjectOwner::closed() const { return state_->scope.closed(); }
bool ObjectOwner::is_current() const noexcept { return state_->scope.is_current(); }
std::size_t ObjectOwner::size() const { std::lock_guard lock(state_->mutex); return state_->entries.size(); }
} // namespace ii::runtime
