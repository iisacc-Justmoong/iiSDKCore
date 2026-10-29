#include "fixtures/observable_module/Model.h"
#include <iiSDKCore/Core/Property/Observable.h>
#include <iostream>
#include <vector>
#include <thread>
#include <stdexcept>
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void basic(){
    void* model=observable_model_create();std::vector<ii::core::ObservableRead> reads;
    bool application=false,library=false;
    const auto value=ii::core::ObservableReadScope::collect([&](ii::core::ObservableRead read){reads.push_back(std::move(read));},[&]{application=ii::core::ObservableReadScope::active();library=observable_model_scope_active();return observable_model_value(model);});
    int notifications=0;ii::core::ObservableSubscription lease;
    if(reads.size()==1)lease=reads.front().subscribe([&]{++notifications;});
    observable_model_set(model,5);const auto updated=observable_model_value(model);observable_model_destroy(model);
    if(value!=6||updated!=10||!application||!library||reads.size()!=1||notifications!=1){
        throw std::runtime_error("Shared module observation failed: application="+std::to_string(application)+", library="+std::to_string(library)+", reads="+std::to_string(reads.size())+", notifications="+std::to_string(notifications));
    }
    std::cout<<"Shared module scoped reads and notifications passed\n";
}
void nested(){
    void* model=observable_model_create();ii::core::Observable<int> local{4};int outer=0,inner=0;
    auto result=ii::core::ObservableReadScope::collect([&](ii::core::ObservableRead){++outer;},[&]{
        (void)local.get();int value=observable_model_collect(model,[](void* value){return static_cast<ii::core::Observable<int>*>(value)->get();},&local,&inner);
        ii::core::ObservableReadScope::pause([&]{check(!observable_model_scope_active(),"A paused scope remained active inside the library");(void)observable_model_value(model);});
        (void)local.get();return value;
    });observable_model_destroy(model);
    check(result==10&&outer==2&&inner==2&&!ii::core::ObservableReadScope::active(),"Nested module reads leaked or lost their owning collector");
}
void exceptions(){
    void* model=observable_model_create();int outer=0;bool caught=false;
    ii::core::ObservableReadScope::collect([&](ii::core::ObservableRead){++outer;},[&]{
        int ignored=0;try{observable_model_collect(model,[](void*)->int{throw std::runtime_error("module callback");},nullptr,&ignored);}catch(const std::runtime_error&){caught=true;}
        check(observable_model_scope_active(),"A module exception lost the outer collector");(void)observable_model_value(model);
    });observable_model_destroy(model);check(caught&&outer==1&&!ii::core::ObservableReadScope::active(),"A module exception contaminated the outer read set");
}
void threads(){
    int main_reads=0,worker_reads=0;bool isolated=false;
    ii::core::ObservableReadScope::collect([&](ii::core::ObservableRead){++main_reads;},[&]{
        std::thread worker([&]{isolated=!observable_model_scope_active();void* model=observable_model_create();ii::core::ObservableReadScope::collect([&](ii::core::ObservableRead){++worker_reads;},[&]{check(observable_model_scope_active(),"The worker library did not use its worker scope");(void)observable_model_value(model);});observable_model_destroy(model);});worker.join();
        void* model=observable_model_create();(void)observable_model_value(model);observable_model_destroy(model);
    });check(isolated&&main_reads==1&&worker_reads==1,"Module collectors did not preserve per-thread isolation");
}
void bounds(){
    void* model=observable_model_create();bool rejected=false;
    const auto recurse=[&](auto&& self,std::size_t remaining)->void{
        ii::core::ObservableReadScope::collect([](ii::core::ObservableRead){},[&]{if(remaining)self(self,remaining-1);else{int ignored=0;(void)observable_model_collect(model,nullptr,nullptr,&ignored);}});
    };
    try{recurse(recurse,ii::core::ObservableReadScope::maximum_depth-1);}catch(const std::length_error&){rejected=true;}
    observable_model_destroy(model);check(rejected&&!ii::core::ObservableReadScope::active(),"Read scope depth admission split across module boundaries");
}
void lifetime(){
    void* model=observable_model_create();std::vector<ii::core::ObservableRead> reads;
    ii::core::ObservableReadScope::collect([&](ii::core::ObservableRead read){reads.push_back(std::move(read));},[&]{(void)observable_model_value(model);});
    check(reads.size()==1,"A module property did not yield a read record");int calls=0;auto lease=reads[0].subscribe([&]{++calls;});observable_model_set(model,7);observable_model_destroy(model);
    bool closed=false;try{(void)reads[0].current_revision();}catch(const std::logic_error&){closed=true;}
    check(calls==1&&closed&&!lease.active(),"A retired module property remained observable or lost its stable read record");
}
int main(int argc,char** argv){try{std::string name=argc>1?argv[1]:"basic";if(name=="basic")basic();else if(name=="nested")nested();else if(name=="exceptions")exceptions();else if(name=="threads")threads();else if(name=="bounds")bounds();else if(name=="lifetime")lifetime();else throw std::runtime_error("Unknown module observation test");std::cout<<"Module observation "<<name<<" passed\n";}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
