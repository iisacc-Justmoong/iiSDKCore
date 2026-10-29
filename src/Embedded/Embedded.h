#pragma once
#include <stdint.h>
#if (defined(_MSVC_LANG) && _MSVC_LANG < 202100L) || (!defined(_MSVC_LANG) && __cplusplus < 202100L)
#error "iiSDKCore Embedded requires C++23"
#endif
namespace ii::embedded {
struct Task final {
    void (*invoke)(void *) noexcept = nullptr;
    void *context = nullptr;
};
struct Ticket final {
    const void *owner = nullptr;
    uint64_t generation = 0;
    uint16_t index = 0;
    explicit constexpr operator bool() const noexcept { return owner != nullptr; }
};
uint32_t contract_version() noexcept;
// One caller owns dispatch and time. Contexts outlive their pending tickets.
// No allocation, OS service, exceptions or interrupt synchronization is
// required.
template <uint16_t Capacity> class EventLoop final {
    static_assert(Capacity > 0 && Capacity <= 1024);
    struct Slot final {
        Task task{};
        uint64_t generation = 0;
        uint32_t due = 0, period = 0;
        bool active = false, immediate = false;
    };
    Slot slots_[Capacity]{};
    uint16_t cursor_ = 0;
    bool polling_ = false;
    Ticket insert(uint32_t due, uint32_t period, Task task, bool immediate) noexcept {
        if (!task.invoke)
            return {};
        for (uint16_t n = 0; n < Capacity; ++n) {
            const auto i = uint16_t((cursor_ + n) % Capacity);
            auto &s = slots_[i];
            if (s.active || s.generation == UINT64_MAX)
                continue;
            s.task = task;
            ++s.generation;
            s.due = due;
            s.period = period;
            s.active = true;
            s.immediate = immediate;
            return {this, s.generation, i};
        }
        return {};
    }

  public:
    constexpr EventLoop() noexcept = default;
    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;
    Ticket post(Task task) noexcept { return insert(0, 0, task, true); }
    Ticket schedule(uint32_t now, uint32_t delay, Task task) noexcept {
        return delay > INT32_MAX ? Ticket{} : insert(now + delay, 0, task, false);
    }
    Ticket every(uint32_t now, uint32_t period, Task task) noexcept {
        return !period || period > INT32_MAX ? Ticket{} : insert(now + period, period, task, false);
    }
    bool cancel(Ticket ticket) noexcept {
        if (ticket.owner != this || ticket.index >= Capacity)
            return false;
        auto &s = slots_[ticket.index];
        if (!s.active || s.generation != ticket.generation)
            return false;
        s.active = false;
        s.task = {};
        return true;
    }
    uint16_t poll(uint32_t now, uint16_t budget = Capacity) noexcept {
        if (polling_)
            return 0;
        polling_ = true;
        uint16_t done = 0;
        for (uint16_t checked = 0; checked < Capacity && done < budget; ++checked) {
            const auto i = cursor_;
            cursor_ = uint16_t((cursor_ + 1) % Capacity);
            auto &s = slots_[i];
            if (!s.active || (!s.immediate && int32_t(now - s.due) < 0))
                continue;
            const auto task = s.task;
            if (s.period) {
                const auto missed = (now - s.due) / s.period;
                s.due += (missed + 1) * s.period;
            } else {
                s.active = false;
                s.task = {};
            }
            ++done;
            task.invoke(task.context);
        }
        polling_ = false;
        return done;
    }
    uint16_t pending() const noexcept {
        uint16_t n = 0;
        for (const auto &s : slots_)
            if (s.active)
                ++n;
        return n;
    }
    void clear() noexcept {
        for (auto &s : slots_) {
            s.active = false;
            s.task = {};
        }
    }
};
} // namespace ii::embedded
