#include <iiSDKCore/Animation/Timeline.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace ii::animation;
using namespace std::chrono_literals;
namespace {
void close(double value,double expected){if(std::abs(value-expected)>1e-10)throw std::runtime_error("Easing sample differs from its polynomial contract");}
}
int main(){try{
    close(ease(Easing::in_cubic,.25),.015625);close(ease(Easing::in_cubic,.5),.125);close(ease(Easing::in_cubic,.75),.421875);
    close(ease(Easing::out_back,.25,1.45),.78203125);close(ease(Easing::out_back,.5,1.45),1.05625);close(ease(Easing::out_back,.75,1.45),1.05234375);
    close(ease(Easing::out_back,.5,0),.875);
    for(auto curve:{Easing::linear,Easing::in_quad,Easing::out_quad,Easing::in_out_quad,Easing::smooth,Easing::in_cubic,Easing::out_back}){
        close(ease(curve,-.1),0);close(ease(curve,0),0);close(ease(curve,1),1);close(ease(curve,1.1),1);
    }
    double previous=0,peak=0;
    for(int i=0;i<=1000;++i){const auto p=i/1000.;const auto value=ease(Easing::in_cubic,p);if(value<previous)throw std::runtime_error("Cubic progression reversed");previous=value;peak=std::max(peak,ease(Easing::out_back,p,1.45));}
    if(peak<=1.07||peak>=1.08)throw std::runtime_error("Back easing lost its authored overshoot");
    Keyframes<double> keys({{0ms,0,Easing::out_back},{1s,1,Easing::linear}});
    close(keys.sample(500ms),1.0876975);
    double sampled=0;Timeline timeline;timeline.append(1s,[&](double p){sampled=p;},Easing::in_cubic);timeline.sample(500ms);close(sampled,.125);
    std::cout<<"EASING_CUBIC_BACK_OK values_only=true overshoot=1.45 timeline=true\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
