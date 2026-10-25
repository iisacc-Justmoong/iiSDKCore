#include <iiSDKCore/Runtime/LoadRouting.h>
#include <iiSDKCore/Runtime/EventLoop.h>
#include <iostream>

int main() {
    using namespace ii::runtime;
    EventLoop api,assets;
    ApplicationLoadBalancer application;
    if(!application.targets().add_target(api,{.name="api",.capacity=8,.group=1}) ||
       !application.targets().add_target(assets,{.name="assets",.capacity=8,.group=2})) return 1;
    if(!application.configure({{.priority=1,.group=1,.host="api.example.com",.path_prefix="/v1/"}},2)) return 2;
    int result=0;
    if(!application.submit({.host="api.example.com",.path="/v1/jobs"},[&]{result+=40;})) return 3;
    if(!application.submit({.host="cdn.example.com",.path="/image.png"},[&]{result+=2;})) return 4;
    api.poll(); assets.poll();
    ClassicLoadBalancer classic;
    if(!classic.targets().add_target(api,{.name="legacy",.capacity=8})) return 5;
    auto request=classic.acquire(ClassicProtocol::http);
    if(!request || !request->post([&]{++result;})) return 6;
    request->complete();
    api.poll();
    if(!application.targets().close(ShutdownMode::drain) || !classic.targets().close(ShutdownMode::drain) || result!=43) return 7;
    std::cout<<"Application and Classic routing complete: result="<<result<<'\n';
}
