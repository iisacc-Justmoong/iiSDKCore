#include "EventLoopIndex.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ii::runtime::detail {
void EventLoopIndex::reserve_slots(std::size_t count) {
    if (count<=positions_.size()) return;
    positions_.resize(count,absent);
}
void EventLoopIndex::reserve_entries(std::size_t count) {
    if (count<=heap_.capacity()) return;
    if (count>positions_.size()) throw std::out_of_range("event index capacity");
    const auto current=heap_.capacity();
    const auto growth=std::min(positions_.size()-current,std::max(std::size_t{1},current));
    heap_.reserve(std::max(count,current+growth));
}
bool EventLoopIndex::less(std::size_t left,std::size_t right) noexcept {
#if defined(IISDKCORE_EVENT_INDEX_METRICS)
    ++comparisons_;
#endif
    const auto& a=heap_[left].key;
    const auto& b=heap_[right].key;
    return a.time<b.time || (a.time==b.time && a.sequence<b.sequence);
}
void EventLoopIndex::exchange(std::size_t left,std::size_t right) noexcept {
    std::swap(heap_[left],heap_[right]);
    positions_[heap_[left].slot]=left;
    positions_[heap_[right].slot]=right;
}
void EventLoopIndex::up(std::size_t position) noexcept {
    while(position) {
        const auto parent=(position-1)/2;
        if(!less(position,parent)) return;
        exchange(position,parent); position=parent;
    }
}
void EventLoopIndex::down(std::size_t position) noexcept {
    while(position<heap_.size()/2) {
        auto child=position*2+1;
        if(child+1<heap_.size() && less(child+1,child)) ++child;
        if(!less(child,position)) return;
        exchange(position,child); position=child;
    }
}
void EventLoopIndex::set(std::size_t slot,Key key) {
    if(slot>=positions_.size()) throw std::out_of_range("event index slot");
    auto position=positions_[slot];
    if(position==absent) {
        if (heap_.size()==heap_.capacity()) reserve_entries(heap_.size()+1);
        position=heap_.size();
        heap_.push_back({slot,key});
        positions_[slot]=position;
        up(position);
        return;
    }
    heap_[position].key=key;
    if(position && less(position,(position-1)/2)) up(position);
    else down(position);
}
bool EventLoopIndex::erase(std::size_t slot) noexcept {
    if(!contains(slot)) return false;
    const auto position=positions_[slot];
    exchange(position,heap_.size()-1);
    heap_.pop_back(); positions_[slot]=absent;
    if(position==heap_.size()) return true;
    if(position && less(position,(position-1)/2)) up(position);
    else down(position);
    return true;
}
bool EventLoopIndex::contains(std::size_t slot) const noexcept {
    return slot<positions_.size() && positions_[slot]!=absent;
}
std::size_t EventLoopIndex::size() const noexcept { return heap_.size(); }
std::optional<EventLoopIndex::Entry> EventLoopIndex::front() const noexcept {
    return heap_.empty() ? std::nullopt : std::optional<Entry>(heap_.front());
}
std::optional<EventLoopIndex::Entry> EventLoopIndex::take() noexcept {
    if(heap_.empty()) return std::nullopt;
    const auto entry=heap_.front();
    (void)erase(entry.slot);
    return entry;
}
}
