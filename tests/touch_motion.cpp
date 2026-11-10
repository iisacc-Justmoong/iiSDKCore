#include <iiSDKCore/Runtime/TouchMotion.h>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace ii::runtime::touch;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool near(double a,double b,double tolerance=1e-6){return std::abs(a-b)<tolerance;}
}
int main(){
    PanVelocity tracker;
    check(tracker.begin(1.),"velocity tracking did not start");
    check(tracker.add({10,-20},1.02)&&tracker.add({10,-20},1.04),"movement samples were rejected");
    const auto velocity=tracker.release(1.04);
    check(near(velocity.x,500)&&near(velocity.y,-1000),"velocity lost direction or units");
    check(tracker.release(1.3).x==0,"a stationary release produced inertia");
    check(!tracker.add({10,10},1.03),"out-of-order sample was accepted");
    check(near(tracker.release(1.04).y,-1000),"invalid sample changed tracking state");
    check(!tracker.begin(std::numeric_limits<double>::quiet_NaN()),"invalid start time was accepted");
    check(tracker.begin(10.),"high-frequency tracker did not start");
    for(int i=1;i<=100;++i)check(tracker.add({1.,0},10.+i*.004),"high-frequency sample was lost");
    check(near(tracker.release(10.4).x,250.,.001),"bounded history changed sustained velocity");
    bool invalid_policy=false;try{(void)KineticMotion(MotionPolicy{.friction=0});}catch(const std::invalid_argument&){invalid_policy=true;}
    check(invalid_policy,"invalid deceleration policy was accepted");

    KineticMotion sixty,one_twenty;
    check(sixty.start({400,-800},2.)&&one_twenty.start({400,-800},2.),"fling failed to start");
    MotionVector a{},b{};
    for(int i=1;i<=30;++i){const auto step=sixty.advance(2.+i/60.);a.x+=step.delta.x;a.y+=step.delta.y;}
    for(int i=1;i<=60;++i){const auto step=one_twenty.advance(2.+i/120.);b.x+=step.delta.x;b.y+=step.delta.y;}
    check(near(a.x,b.x)&&near(a.y,b.y)&&a.x>0&&a.y<0,"motion depends on display frequency or reversed direction");
    sixty.block(true,false);const auto blocked=sixty.advance(2.6);
    check(blocked.delta.x==0&&blocked.delta.y<0,"boundary did not stop only its own axis");
    sixty.cancel();check(!sixty.active()&&sixty.advance(2.7).delta.y==0,"cancellation left motion active");
    check(!sixty.start({1,1},3.),"a nearly stationary gesture started inertia");
    check(sixty.start({500,0},4.),"valid interruption fixture failed");
    const auto suspended=sixty.advance(5.);
    check(!suspended.active&&suspended.delta.x==0,"a long frame suspension caused a jump");
    check(!sixty.start({std::numeric_limits<double>::infinity(),0},6.),"nonfinite velocity was accepted");
}
