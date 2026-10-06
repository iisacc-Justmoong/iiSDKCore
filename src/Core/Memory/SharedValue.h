#pragma once
#include "../Result/Result.h"
#include <atomic>
#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ii::core {
template <class T> struct Snapshot final {
  std::shared_ptr<const T> value;
  std::uint64_t revision = 0;
  [[nodiscard]] const T &get() const noexcept { return *value; }
};
// Copies share storage; writes detach automatically. T must own its mutable
// contents: copying T does not clone shared mutable pointees.
template <std::copy_constructible T> class SharedValue final {
public:
  explicit SharedValue(T value)
      : block_(std::make_shared<Block>(std::move(value))) {}
  SharedValue(const SharedValue &other) {
    auto [block, revision] = other.shared_state();
    block_ = std::move(block);
    revision_ = revision;
  }
  SharedValue &operator=(const SharedValue &other) {
    if (this == &other)
      return *this;
    auto [block, revision] = other.shared_state();
    (void)revision;
    std::shared_ptr<Block> retired;
    {
      std::lock_guard lock(mutex_);
      require_readable();
      if (revision_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Shared value revision exhausted");
      retired = std::exchange(block_, std::move(block));
      ++revision_;
    }
    return *this;
  }
  [[nodiscard]] Snapshot<T> snapshot() const {
    std::lock_guard lock(mutex_);
    require_readable();
    return observe_locked();
  }
  // Short serialized write. Unique, unpublished storage is reused only for a
  // noexcept callback; throwing callbacks receive a private rollback candidate.
  template <class Edit>
    requires std::invocable<Edit, T &> &&
             std::same_as<std::invoke_result_t<Edit, T &>, void>
  [[nodiscard]] Result<void> modify(Edit &&edit) {
    std::shared_ptr<Block> next, retired;
    std::unique_lock lock(mutex_);
    if (editing_)
      return fail(ErrorCode::busy, "Recursive shared value write");
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
      return fail(ErrorCode::full, "Shared value revision exhausted");
    bool reuse = false;
    if constexpr (std::is_nothrow_invocable_v<Edit, T &>) {
      // A published alias can be resurrected by weak_ptr::lock even when
      // use_count()==1. RMW observes publication before permitting reuse.
      if (block_.use_count() == 1 &&
          !block_->published.exchange(true, std::memory_order_acq_rel))
        reuse = true;
    }
    editing_ = true;
    try {
      if (!reuse)
        next = std::make_shared<Block>(block_->value);
      std::invoke(std::forward<Edit>(edit),
                  reuse ? block_->value : next->value);
    } catch (...) {
      editing_ = false;
      if (reuse)
        block_->published.store(false, std::memory_order_release);
      auto cause = std::current_exception();
      lock.unlock();
      return fail(ErrorCode::failed, "Shared value modification failed",
                  std::move(cause));
    }
    if (reuse)
      block_->published.store(false, std::memory_order_release);
    else
      retired = std::exchange(block_, std::move(next));
    ++revision_;
    editing_ = false;
    lock.unlock();
    return {};
  }
  // Optimistic transaction: edits run outside the mutex; no retry repeats side
  // effects. This preserves the pre-existing update contract.
  template <class Edit>
    requires std::invocable<Edit, T &>
  [[nodiscard]] Result<void> update(Edit &&edit) {
    Snapshot<T> base;
    {
      std::lock_guard lock(mutex_);
      if (editing_)
        return fail(ErrorCode::busy, "Recursive shared value write");
      base = observe_locked();
    }
    std::shared_ptr<Block> next;
    try {
      next = std::make_shared<Block>(base.get());
      std::invoke(std::forward<Edit>(edit), next->value);
    } catch (...) {
      return fail(ErrorCode::failed, "Shared value edit failed",
                  std::current_exception());
    }
    std::shared_ptr<Block> retired;
    {
      std::lock_guard lock(mutex_);
      if (editing_)
        return fail(ErrorCode::busy, "Recursive shared value write");
      if (std::addressof(block_->value) != base.value.get() ||
          revision_ != base.revision)
        return fail(ErrorCode::busy,
                    "Shared value changed during edit; retry explicitly");
      if (revision_ == std::numeric_limits<std::uint64_t>::max())
        return fail(ErrorCode::full, "Shared value revision exhausted");
      retired = std::exchange(block_, std::move(next));
      ++revision_;
    }
    return {};
  }

private:
  struct Block final {
    T value;
    std::atomic<bool> published{false};
    explicit Block(T value) : value(std::move(value)) {}
  };
  // Recursive locking is solely for detecting invalid reentry instead of
  // deadlocking; callbacks cannot read/write the owning instance recursively.
  mutable std::recursive_mutex mutex_;
  std::shared_ptr<Block> block_;
  std::uint64_t revision_ = 0;
  bool editing_ = false;
  void require_readable() const {
    if (editing_)
      throw std::logic_error("Shared value cannot be observed or assigned from "
                             "its modification callback");
  }
  std::pair<std::shared_ptr<Block>, std::uint64_t> shared_state() const {
    std::lock_guard lock(mutex_);
    require_readable();
    return {block_, revision_};
  }
  Snapshot<T> observe_locked() const {
    block_->published.store(true, std::memory_order_release);
    return {std::shared_ptr<const T>(block_, std::addressof(block_->value)),
            revision_};
  }
};
} // namespace ii::core
