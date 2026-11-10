#include "../src/Runtime/EventLoopIndex.h"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>

using ii::runtime::detail::EventLoopIndex;
using namespace std::chrono_literals;

namespace {
void check(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        constexpr std::size_t count=8192;
        EventLoopIndex index;
        index.reserve_slots(count);
        const auto origin=EventLoopIndex::Time{};
        for (std::size_t i=0;i<count;++i)
            index.set(i,{origin+std::chrono::nanoseconds(count-i),i});
        const auto first=index.front();
        check(index.size()==count && first && first->slot==count-1,"earliest deadline is indexed");
        index.set(0,{origin-1ns,0});
        const auto moved=index.front(); const auto taken=index.take();
        check(moved && taken && moved->slot==0 && taken->slot==0,"updating a deadline moves the entry");
        for (std::size_t i=1;i<count;i+=2) check(index.erase(i),"remove an arbitrary entry");
        check(!index.erase(1) && !index.contains(0),"removed entries remain absent");
        EventLoopIndex::Key previous{origin-1ns,0};
        while (const auto item=index.take()) {
            check(previous.time<item->key.time ||
                  (previous.time==item->key.time && previous.sequence<=item->key.sequence),
                  "remaining deadlines retain stable order");
            previous=item->key;
        }
        check(index.comparisons()<count*100,"indexed insertion, updates and removals avoid full scans");
        index.set(5,{origin+1ns,8}); index.set(6,{origin+1ns,7});
        const auto earlier=index.take(),later=index.take();
        check(earlier && later && earlier->slot==6 && later->slot==5 && !index.front(),
              "equal deadlines use sequence order and slots can be reused");
        EventLoopIndex growing;
        growing.reserve_slots(2);
        growing.set(0,{origin+1ns,1}); growing.set(1,{origin+2ns,2});
        growing.reserve_slots(128);
        growing.set(127,{origin,3}); growing.set(0,{origin+3ns,1});
        const auto a=growing.take(),b=growing.take(),c=growing.take();
        check(a && b && c && a->slot==127 && b->slot==1 && c->slot==0,
              "growth and later deadline updates retain existing entries");
        std::cout<<"event index ordering and bounded comparisons passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n'; return 1;
    }
}
