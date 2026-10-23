#include <iiSDKCore/Core/Property/Observable.h>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>
using namespace ii::core;
#define CHECK(x) do{if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
int main(){
    Observable<int> value{1};std::vector<int> seen;
    auto first=value.observe([&](const int& n,std::uint64_t){seen.push_back(n);});
    CHECK(!value.set(1)&&value.revision()==0);CHECK(value.set(2)&&seen==std::vector<int>{2});
    first.reset();CHECK(value.set(3)&&seen.size()==1);
    auto recursive=value.observe([&](const int& n,std::uint64_t){if(n==4)value.set(5);});
    auto last=value.observe([&](const int& n,std::uint64_t){seen.push_back(n);});
    value.set(4);CHECK((seen==std::vector<int>{2,4,5})&&value.get()==5);
    bool wrong_thread=false;std::thread worker([&]{try{value.set(99);}catch(const std::logic_error&){wrong_thread=true;}});worker.join();CHECK(wrong_thread&&value.get()==5);
    ObservableSubscription retired;int calls=0;
    {Observable<std::string> text{"ready"};retired=text.observe([&](const auto&,auto){++calls;},true);CHECK(calls==1&&retired.active());}
    CHECK(!retired.active());retired.reset();
    Observable<int> cancel{0};ObservableSubscription second;
    auto remover=cancel.observe([&](const auto&,auto){second.reset();});second=cancel.observe([&](const auto&,auto){++calls;});cancel.set(1);CHECK(calls==1);
    Observable<int> throws{0};auto bad=throws.observe([](const auto&,auto){throw std::runtime_error("observer");});
    auto good=throws.observe([&](const auto&,auto){++calls;});bool failed=false;
    try{throws.set(1);}catch(const std::runtime_error&){failed=true;}CHECK(failed&&calls==2&&throws.get()==1);bad.reset();CHECK(throws.set(2));
    Observable<int> cycle{0};auto loop=cycle.observe([&](const int& n,auto){cycle.set(n+1);});
    bool bounded=false;try{cycle.set(1);}catch(const std::length_error&){bounded=true;}CHECK(bounded&&cycle.get()==Observable<int>::maximum_notifications);
    std::cout<<"observable contracts passed\n";
}
