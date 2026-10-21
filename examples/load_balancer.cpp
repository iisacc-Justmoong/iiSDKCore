#include <iiSDKCore/Runtime/LoadBalancer.h>
#include <atomic>
#include <iostream>

int main() {
    using namespace ii::runtime;
    // Declare borrowed executors first so their workers outlive the balancer.
    Runtime interactive({.workers=2,.capacity=16});
    Runtime batch({.workers=4,.capacity=32});
    LoadBalancer compute({.max_targets=4,.capacity=48});
    const auto small=compute.add_target(interactive,{.name="interactive-cpu",.capacity=16});
    const auto large=compute.add_target(batch,{.name="batch-cpu",.capacity=32});
    if(!small || !large) return 1;
    std::atomic<int> sum=0;
    for(int i=1;i<=12;++i) {
        if(!compute.post([&sum,i]{sum.fetch_add(i*i,std::memory_order_relaxed);})) return 2;
    }
    // Removal stops new admission and keeps every already accepted job.
    if(!compute.remove_target(*small,ShutdownMode::drain)) return 3;
    if(!compute.close(ShutdownMode::drain)) return 4;
    const auto statistics=compute.stats();
    if(statistics.completed!=12 || statistics.cancelled || sum!=650) return 5;
    std::cout<<"native load balancing complete: jobs="<<statistics.completed<<" sum="<<sum.load()<<'\n';
    return 0;
}
