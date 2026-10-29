#include <iiSDKCore/Core/Property/Observable.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace ii::core;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Model final {Observable<int> count{3};int twice()const{return count.get()*2;}};
void reads(){
    Model model;std::vector<ObservableRead> reads;
    auto result=ObservableReadScope::collect([&](ObservableRead read){reads.push_back(std::move(read));},[&]{return model.twice();});
    check(result==6&&reads.size()==1&&reads[0].revision==0&&reads[0].identity==model.count.read_dependency().identity,"A native getter did not expose its exact source dependency");
    int changed=0;auto lease=reads[0].subscribe([&]{++changed;});model.count.set(5);
    check(changed==1&&reads[0].current_revision()==1&&!ObservableReadScope::active(),"A collected read did not use the original observation contract");
}
void nested(){
    Model a,b;std::vector<ObservableRead> outer,inner;
    ObservableReadScope::collect([&](ObservableRead read){outer.push_back(std::move(read));(void)a.count.get();},[&]{
        (void)a.twice();ObservableReadScope::collect([&](ObservableRead read){inner.push_back(std::move(read));},[&]{(void)b.twice();});
        ObservableReadScope::pause([&]{(void)b.twice();});(void)a.twice();
    });
    check(outer.size()==2&&inner.size()==1&&outer[0].identity!=inner[0].identity,"Nested collectors, paused calls or recorder reads leaked into the outer read set");
}
void exceptions(){
    Model model;int calls=0;
    ObservableReadScope::collect([&](ObservableRead){++calls;},[&]{
        try{ObservableReadScope::collect([](ObservableRead){throw std::runtime_error("collector");},[&]{return model.twice();});}catch(const std::runtime_error&){}
        (void)model.twice();try{ObservableReadScope::pause([]{throw std::runtime_error("paused");});}catch(const std::runtime_error&){}(void)model.twice();
    });
    check(calls==2&&!ObservableReadScope::active(),"An exceptional nested read did not restore its caller's collection scope");
    auto closing=std::make_unique<Observable<int>>(7);bool closed=false;
    try{ObservableReadScope::collect([&](ObservableRead){closing.reset();},[&]{return closing->get();});}catch(const std::logic_error&){closed=true;}
    check(closed&&!ObservableReadScope::active(),"A collector destroyed a property while its get still accessed the property object");
}
void lifetime(){
    ObservableRead read;ObservableSubscription lease;int calls=0;
    {Observable<int> source{2};read=source.read_dependency();lease=read.subscribe([&]{++calls;});Observable<int> moved{std::move(source)};moved.set(4);check(calls==1&&read.current_revision()==1,"Moving a property changed its read identity");}
    bool closed=false,subscribe_closed=false;try{(void)read.current_revision();}catch(const std::logic_error&){closed=true;}
    try{auto next=read.subscribe([]{});}catch(const std::logic_error&){subscribe_closed=true;}
    check(closed&&subscribe_closed&&!lease.active(),"A retained read record dereferenced a destroyed property or accepted new work");
}
void threads(){
    Model model;int outer=0,inner=0;bool wrong=false;
    ObservableReadScope::collect([&](ObservableRead){++outer;},[&]{
        std::thread worker([&]{Model local;check(!ObservableReadScope::active(),"Read collection propagated across threads");ObservableReadScope::collect([&](ObservableRead){++inner;},[&]{(void)local.twice();});try{(void)model.twice();}catch(const std::logic_error&){wrong=true;}});worker.join();(void)model.twice();
    });check(outer==1&&inner==1&&wrong,"Scoped native reads broke execution-domain isolation");
}
void bounds(){
    auto recurse=[&](auto&& self,int depth)->void{ObservableReadScope::collect([](ObservableRead){},[&]{if(depth)self(self,depth-1);});};
    recurse(recurse,ObservableReadScope::maximum_depth-1);bool rejected=false;
    try{recurse(recurse,ObservableReadScope::maximum_depth);}catch(const std::length_error&){rejected=true;}
    check(rejected&&!ObservableReadScope::active(),"Read collector depth was unbounded or leaked after refusal");
}
void returns(){
    struct Value final {int count;explicit Value(int n):count(n){}Value(const Value&)=delete;Value(Value&&)=delete;};
    Model model;auto value=ObservableReadScope::collect([](ObservableRead){},[&]{return Value(model.twice());});
    const int& reference=ObservableReadScope::collect([](ObservableRead){},[&]()->const int&{return model.count.get();});
    check(value.count==6&&&reference==&model.count.get(),"Read collection changed C++ value elision or reference semantics");
}
int main(int argc,char** argv){try{std::string name=argc>1?argv[1]:"reads";if(name=="reads")reads();else if(name=="nested")nested();else if(name=="exceptions")exceptions();else if(name=="lifetime")lifetime();else if(name=="threads")threads();else if(name=="bounds")bounds();else if(name=="returns")returns();else throw std::runtime_error("Unknown read collector test");std::cout<<"Scoped observable reads "<<name<<" passed\n";}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
