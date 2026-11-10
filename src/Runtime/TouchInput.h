#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace ii::runtime::touch {

// Samples are supplied by a platform or hardware backend in surface coordinates.
struct TouchSample final {
    std::uint64_t identity = 0;
    double x = 0, y = 0, timestamp = 0, pressure = 1;
    double tilt_x = 0, tilt_y = 0, rotation = 0;
    std::uint32_t tap_count = 0;
    bool pen = false;
};
enum class TouchEventKind { pointer_down, pointer_move, pointer_up, pointer_cancel, pan, pinch };
struct TouchEvent final {
    TouchEventKind kind = TouchEventKind::pointer_move;
    TouchSample sample;
    // Pan is the actual content displacement; pinch is the incremental ratio.
    double dx = 0, dy = 0, scale = 1;
};
struct TouchBatch final {
    std::array<TouchEvent, 36> events{};
    std::size_t size = 0;
    [[nodiscard]] const TouchEvent& operator[](std::size_t index) const noexcept { return events[index]; }
};

// One instance is owned by one input surface and used on its event thread.
class TouchRouter final {
    enum class Mode { idle, pending, pointer, scroll, pair, suppressed };
    Mode mode_ = Mode::idle;
    std::array<std::uint64_t, 16> active_{};
    std::size_t active_count_ = 0, pending_count_ = 0;
    std::array<TouchSample, 32> pending_{};
    TouchSample first_{}, last_{}, second_{};
    double pair_x_ = 0, pair_y_ = 0, pair_distance_ = 0;
    double initial_x_ = 0, initial_y_ = 0, initial_distance_ = 0;
    double threshold_ = 4.0;
    std::uint64_t revision_ = 0;
    bool pair_active_ = false;

    [[nodiscard]] static bool valid(const TouchSample& value) noexcept {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::abs(value.x) <= 1e9 && std::abs(value.y) <= 1e9 &&
               std::isfinite(value.timestamp) && value.timestamp >= 0 &&
               std::isfinite(value.pressure) && value.pressure >= 0 && value.pressure <= 1 &&
               std::isfinite(value.tilt_x) && std::isfinite(value.tilt_y) && std::isfinite(value.rotation);
    }
    [[nodiscard]] bool active(std::uint64_t id) const noexcept {
        return std::find(active_.begin(), active_.begin() + active_count_, id) != active_.begin() + active_count_;
    }
    void remove(std::uint64_t id) noexcept {
        auto end = active_.begin() + active_count_;
        const auto found = std::find(active_.begin(), end, id);
        if (found != end) { std::move(found + 1, end, found); --active_count_; }
    }
    static void append(TouchBatch& batch, TouchEventKind kind, const TouchSample& point,
                       double dx = 0, double dy = 0, double scale = 1) noexcept {
        batch.events[batch.size++] = {kind, point, dx, dy, scale};
    }
    void replay(TouchBatch& batch) noexcept {
        append(batch, TouchEventKind::pointer_down, first_);
        for (std::size_t i = 0; i < pending_count_; ++i) append(batch, TouchEventKind::pointer_move, pending_[i]);
        pending_count_ = 0;
        mode_ = Mode::pointer;
    }
    void reset() noexcept { mode_ = Mode::idle; active_count_ = pending_count_ = 0; pair_active_ = false; }
public:
    explicit TouchRouter(double threshold = 4.0) : threshold_(threshold) {
        if (!std::isfinite(threshold) || threshold <= 0 || threshold > 1024)
            throw std::invalid_argument("Invalid touch movement threshold");
    }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] bool empty() const noexcept { return active_count_ == 0; }
    [[nodiscard]] std::optional<TouchBatch> down(const TouchSample& point, bool scrollable) noexcept {
        if (!valid(point) || active(point.identity) || active_count_ == active_.size()) return {};
        TouchBatch batch;
        ++revision_;
        active_[active_count_++] = point.identity;
        // A pen owns its stroke while any incidental fingers remain ignored.
        if (mode_ != Mode::idle && first_.pen && active(first_.identity)) return batch;
        if (mode_ != Mode::idle && point.pen) {
            if (mode_ == Mode::pointer) append(batch, TouchEventKind::pointer_cancel, last_);
            first_ = last_ = point; pending_count_ = 0; pair_active_ = false;
            mode_ = scrollable ? Mode::pending : Mode::pointer;
            if (!scrollable) append(batch, TouchEventKind::pointer_down, point);
            return batch;
        }
        if (mode_ == Mode::idle) {
            first_ = last_ = point; pending_count_ = 0;
            mode_ = scrollable ? Mode::pending : Mode::pointer;
            if (!scrollable) append(batch, TouchEventKind::pointer_down, point);
        } else if (active_count_ == 2 && mode_ != Mode::suppressed) {
            if (mode_ == Mode::pointer) append(batch, TouchEventKind::pointer_cancel, last_);
            pending_count_ = 0; second_ = point;
            initial_x_ = pair_x_ = (last_.x + second_.x) / 2;
            initial_y_ = pair_y_ = (last_.y + second_.y) / 2;
            initial_distance_ = pair_distance_ = std::hypot(last_.x - second_.x, last_.y - second_.y);
            pair_active_ = false; mode_ = Mode::pair;
        } else { mode_ = Mode::suppressed; pending_count_ = 0; }
        return batch;
    }
    [[nodiscard]] std::optional<TouchBatch> move(const TouchSample& point,
                                                  std::optional<TouchSample> other = {}) noexcept {
        if (!valid(point) || !active(point.identity) || (other && (!valid(*other) || !active(other->identity)))) return {};
        TouchBatch batch;
        if (mode_ == Mode::suppressed) return batch;
        if (mode_ == Mode::pair) {
            auto first = last_, second = second_;
            if (point.identity == first_.identity) first = point;
            else if (point.identity == second_.identity) second = point;
            else return {};
            if (other) {
                if (other->identity == first_.identity && point.identity != first_.identity) first = *other;
                else if (other->identity == second_.identity && point.identity != second_.identity) second = *other;
                else return {};
            }
            const double x = (first.x + second.x) / 2, y = (first.y + second.y) / 2;
            const double distance = std::hypot(first.x - second.x, first.y - second.y);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(distance)) return {};
            const double scale = pair_distance_ > 0 ? distance / pair_distance_ : 1;
            if (!std::isfinite(scale)) return {};
            last_ = first; second_ = second; ++revision_;
            if (!pair_active_ && (std::hypot(x - initial_x_, y - initial_y_) >= threshold_ ||
                                  std::abs(distance - initial_distance_) >= threshold_)) pair_active_ = true;
            if (pair_active_) {
                auto center = point; center.x = x; center.y = y; center.pen = false;
                if (x != pair_x_ || y != pair_y_) append(batch, TouchEventKind::pan, center, x - pair_x_, y - pair_y_);
                if (pair_distance_ > 0 && distance > 0 && distance != pair_distance_)
                    append(batch, TouchEventKind::pinch, center, 0, 0, scale);
                pair_x_ = x; pair_y_ = y; pair_distance_ = distance;
            }
            return batch;
        }
        if (point.identity != first_.identity) return batch;
        ++revision_;
        if (mode_ == Mode::pending) {
            pending_[pending_count_++] = point;
            const auto dx = point.x - first_.x, dy = point.y - first_.y;
            if (std::hypot(dx, dy) >= threshold_) {
                if (std::abs(dy) > std::abs(dx)) {
                    mode_ = Mode::scroll; pending_count_ = 0;
                    auto anchor=first_;anchor.timestamp=point.timestamp;
                    append(batch, TouchEventKind::pan, anchor, dx, dy);
                } else replay(batch);
            } else if (pending_count_ == pending_.size()) replay(batch);
        } else if (mode_ == Mode::scroll) {
            if (point.x != last_.x || point.y != last_.y) {
                auto anchor=first_;anchor.timestamp=point.timestamp;
                append(batch, TouchEventKind::pan, anchor, point.x - last_.x, point.y - last_.y);
            }
        } else if (mode_ == Mode::pointer) append(batch, TouchEventKind::pointer_move, point);
        last_ = point;
        return batch;
    }
    [[nodiscard]] std::optional<TouchBatch> up(const TouchSample& point) noexcept {
        if (!valid(point) || !active(point.identity)) return {};
        TouchBatch batch;
        ++revision_;
        if (mode_ == Mode::pending && point.identity == first_.identity) { replay(batch); append(batch, TouchEventKind::pointer_up, point); }
        else if (mode_ == Mode::pointer && point.identity == first_.identity) append(batch, TouchEventKind::pointer_up, point);
        if (mode_ == Mode::pair) mode_ = Mode::suppressed;
        remove(point.identity);
        if (active_count_ == 0) reset();
        else if (point.identity == first_.identity) { mode_ = Mode::suppressed; pending_count_ = 0; }
        return batch;
    }
    [[nodiscard]] std::optional<TouchBatch> cancel(std::uint64_t identity) noexcept {
        if (!active(identity)) return {};
        TouchBatch batch;
        ++revision_;
        const bool primary = identity == first_.identity;
        if (primary && mode_ == Mode::pointer) append(batch, TouchEventKind::pointer_cancel, last_);
        if (mode_ == Mode::pair) mode_ = Mode::suppressed;
        remove(identity);
        if (active_count_ == 0) reset();
        else if (primary) { mode_ = Mode::suppressed; pending_count_ = 0; }
        return batch;
    }
    [[nodiscard]] std::optional<TouchBatch> cancel() noexcept {
        TouchBatch batch;
        ++revision_;
        if (mode_ == Mode::pointer) append(batch, TouchEventKind::pointer_cancel, last_);
        reset();
        return batch;
    }
};
}
