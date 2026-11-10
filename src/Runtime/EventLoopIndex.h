#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ii::runtime::detail {
// Internal indexed scheduling storage; no executor or platform dependencies.
class EventLoopIndex final {
public:
    using Time=std::chrono::steady_clock::time_point;
    struct Key final { Time time; std::uint64_t sequence; };
    struct Entry final { std::size_t slot; Key key; };
    void reserve_slots(std::size_t);
    void reserve_entries(std::size_t);
    void set(std::size_t,Key);
    bool erase(std::size_t) noexcept;
    [[nodiscard]] bool contains(std::size_t) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::optional<Entry> front() const noexcept;
    [[nodiscard]] std::optional<Entry> take() noexcept;
#if defined(IISDKCORE_EVENT_INDEX_METRICS)
    [[nodiscard]] std::uint64_t comparisons() const noexcept { return comparisons_; }
#endif
private:
    static constexpr std::size_t absent=static_cast<std::size_t>(-1);
    [[nodiscard]] bool less(std::size_t,std::size_t) noexcept;
    void exchange(std::size_t,std::size_t) noexcept;
    void up(std::size_t) noexcept;
    void down(std::size_t) noexcept;
    std::vector<Entry> heap_;
    std::vector<std::size_t> positions_;
#if defined(IISDKCORE_EVENT_INDEX_METRICS)
    std::uint64_t comparisons_=0;
#endif
};
} // namespace ii::runtime::detail
