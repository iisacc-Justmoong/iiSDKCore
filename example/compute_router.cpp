#include <iiSDKCore/Runtime/ComputeRouter.h>
#include <atomic>
#include <iostream>

int main() {
    using namespace ii::runtime;
    Runtime interactive({.workers=2,.capacity=16}),batch({.workers=4,.capacity=32});
    ComputeRouter compute;
    const auto a=compute.add_zone({.name="interactive"});
    const auto b=compute.add_zone({.name="batch",.limits={.jobs=16}});
    if(!a || !b) return 1;
    if(!compute.add_target(interactive,*a) || !compute.add_target(batch,*b)) return 2;
    auto scope=compute.add_scope({.name="export",.zones={*b},.limits={.jobs=16}});
    if(!scope) return 3;
    std::atomic<int> result=0;
    for(int i=1;i<=8;++i) if(!compute.submit(*scope,[&result,i]{result+=i;})) return 4;
    if(!compute.configure_scope(*scope,{.name="export"})) return 5;
    if(!compute.close(ShutdownMode::drain)) return 6;
    const auto snapshot=compute.monitor();
    if(result!=36 || snapshot.load.completed!=8) return 7;
    std::cout<<"dynamic compute routing complete: jobs="<<snapshot.load.completed<<" sum="<<result.load()<<'\n';
}
