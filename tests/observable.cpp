#include <iiSDKCore/Core/Property/Observable.h>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>
using namespace ii::core;
#define CHECK(x) do{if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
template<class T>concept CanObserve=requires(T& value){value.observe([](const auto&,std::uint64_t){});};
template<class T>concept CanSet=requires(T& value){value.set(1);};
static_assert(CanObserve<const Observable<int>>,"Readonly Observable must allow observation");
static_assert(!CanSet<const Observable<int>>,"Readonly observation must not grant mutation");
int main(){
    {
        struct Record final {std::string name;int count;};
        Observable<Record> record{{"Initial",1}};int calls=0;
        auto watch=std::as_const(record).observe([&](const Record& value,auto){++calls;CHECK(value.name=="Changed");});
        CHECK(record.set({"Changed",2})&&record.set({"Changed",2})&&record.revision()==2&&calls==2);
        Record target{"Referenced",3};Observable<std::reference_wrapper<const Record>> reference{std::cref(target)};
        int shown=0;auto changed=std::as_const(reference).observe([&](auto value,auto){shown=value.get().count;});
        target.count=7;CHECK(reference.set(std::cref(target))&&shown==7&&reference.revision()==1);
        int scalar=3;Observable<std::reference_wrapper<const int>> comparable{std::cref(scalar)};
        scalar=7;CHECK(!comparable.set(std::cref(scalar))&&comparable.get().get()==7&&comparable.revision()==0);
    }
    {
        Observable<int> source{7};const auto& readonly=source;std::vector<int> changes;
        auto lease=readonly.observe([&](int value,std::uint64_t){changes.push_back(value);},true);
        CHECK(readonly.get()==7&&readonly.revision()==0&&changes==std::vector<int>{7});
        source.set(9);CHECK((changes==std::vector<int>{7,9})&&readonly.revision()==1);
        bool wrong=false;std::thread other([&]{try{auto bad=readonly.observe([](auto,auto){});}catch(const std::logic_error&){wrong=true;}});other.join();
        CHECK(wrong&&source.get()==9);lease.reset();source.set(11);CHECK(changes.size()==2);
        Observable<int> moved{std::move(source)};const auto& moved_readonly=moved;
        bool retired=false;try{auto bad=readonly.observe([](auto,auto){});}catch(const std::logic_error&){retired=true;}
        auto active=moved_readonly.observe([&](auto value,auto){changes.push_back(value);});
        CHECK(retired&&moved.set(13)&&changes.back()==13&&active.active());
    }
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
