#include <iiSDKCore/Runtime/ManagedMemory.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace ii::runtime;
namespace {
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
void ownership_and_transfer(){
    MemoryPool first(256),second(64);
    auto block=first.allocate(48,64).value();
    check(reinterpret_cast<std::uintptr_t>(block.bytes().data())%64==0,"allocation must honor alignment");
    for(std::size_t i=0;i<block.size();++i)block.bytes()[i]=std::byte(i);
    check(first.stats().used==48&&first.stats().allocations==1,"pool must count a live allocation");
    check(!first.set_limit(47)&&first.stats().limit==256,"failed limit change preserves prior budget");
    check(bool(block.transfer(second)),"allocation moves between pools");
    check(first.stats().used==0&&second.stats().used==48&&block.size()==48,"transfer must return the original allocation");
    for(std::size_t i=0;i<block.size();++i)check(block.bytes()[i]==std::byte(i),"transfer lost contents");
    check(!block.resize(80)&&block.size()==48&&second.stats().used==48,"failed resize must preserve ownership and contents");
    check(bool(second.set_limit(128))&&bool(block.resize(80)),"resize with temporary headroom succeeds");
    for(std::size_t i=0;i<48;++i)check(block.bytes()[i]==std::byte(i),"resize lost old contents");
    MemoryPool tight(10);check(!block.transfer(tight)&&block.size()==80&&second.stats().used==80,"failed transfer leaves source intact");
    block.reset();check(second.stats().used==0&&second.stats().allocations==0,"reset returns storage");
    check(bool(second.set_limit(0))&&!second.allocate(1),"zero limit forbids allocation");
    check(bool(second.set_limit(128))&&bool(second.allocate(1)),"budget can be reopened");
    check(!first.allocate(1,3)&&!first.allocate(0),"invalid size and alignment rejected");
}
void pool_lifetime(){
    MemoryBlock retained;
    {MemoryPool pool(16);retained=std::move(pool.allocate(16).value());retained.bytes()[0]=std::byte{42};}
    check(retained.size()==16&&retained.bytes()[0]==std::byte{42},"block must outlive its pool handle");retained.reset();
}
void concurrent_pool(){
    MemoryPool pool(8*128);std::vector<std::thread> workers;std::atomic<bool> ok=true;
    for(int t=0;t<8;++t)workers.emplace_back([&]{for(int i=0;i<500;++i){auto b=pool.allocate(128);if(!b){ok=false;return;}b->bytes()[0]=std::byte{1};}});
    for(auto& worker:workers)worker.join();
    const auto result=pool.stats();check(ok&&result.used==0&&result.allocations==0&&result.peak<=result.limit,"concurrent allocation accounting must settle");
}
}
int main(){try{ownership_and_transfer();pool_lifetime();concurrent_pool();std::cout<<"MANAGED_MEMORY_OK\n";}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
