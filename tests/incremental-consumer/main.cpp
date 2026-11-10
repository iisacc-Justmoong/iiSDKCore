#include <iiSDKCore/Runtime/ComputeRouter.h>
#include <iiSDKCore/Runtime/FrameLoop.h>
#include <iiSDKCore/Runtime/QueryStream.h>
#include <optional>

using namespace ii::runtime;
struct Query final { unsigned fields=0; };
struct Change final { int value=0; unsigned fields=0; };

int main() {
    EventLoop loop;
    QueryStream<Query,Change,int> stream([](const Query&,const Change& change) {
        return std::optional<int>(change.value);
    });
    int received=0,other=0;
    auto first=stream.subscribe(loop,Query{1},[&](auto event){received=event.value;});
    auto second=stream.subscribe(loop,Query{2},[&](auto){++other;});
    if(!first || !second) return 1;
    const auto published=stream.publish_changed(Change{42,1},[](const Query& query,const Change& change) {
        return (query.fields & change.fields)!=0;
    });
    if(!published || published->enqueued!=1 || published->unaffected!=1 || loop.poll()!=1 || received!=42 || other) return 2;
    Runtime runtime({.workers=1,.capacity=4,.queues=2});
    if(runtime.stats().queues!=2 || runtime.queue_stats().size()!=2) return 3;
    ComputeRouter router;
    const auto zone=router.add_zone({.name="consumer"});
    if(!zone) return 3;
    const auto id=router.add_target(runtime,*zone);
    if(!id) return 4;
    const auto view=router.query_target({.target=*id,.runtime=true});
    if(!view || !view->runtime || view->load || view->runtime->stats.workers!=1) return 5;
    if(FrameStats{}.schedule_rebuilds) return 6;
    if(LoopStats{}.io_candidates) return 6;
    if(!first->close() || !second->close() || !stream.close() || !router.close(ShutdownMode::drain) || !runtime.join()) return 7;
    return 0;
}
