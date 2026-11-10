#include <iiSDKCore/Runtime/TouchInput.h>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace ii::runtime::touch;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool near(double a,double b){return std::abs(a-b)<1e-9;}
TouchSample sample(std::uint64_t id,double x,double y,bool pen=false){
    TouchSample s; s.identity=id;s.x=x;s.y=y;s.pressure=.4;s.timestamp=1;s.pen=pen;return s;
}
}
int main(){
    TouchRouter router;
    auto output=router.down(sample(1,30,40),false);
    check(output&&output->size==1&&(*output)[0].kind==TouchEventKind::pointer_down,"drawing press delayed");
    output=router.move(sample(1,35,42));
    check(output&&output->size==1&&(*output)[0].kind==TouchEventKind::pointer_move&&near((*output)[0].sample.pressure,.4),"pointer sample lost");
    output=router.up(sample(1,35,42));check(output&&output->size==1&&(*output)[0].kind==TouchEventKind::pointer_up,"pointer release lost");

    output=router.down(sample(2,10,70),true);check(output&&output->size==0,"scroll candidate pressed before decision");
    output=router.move(sample(2,11,62));check(output&&output->size==1&&(*output)[0].kind==TouchEventKind::pan&&near((*output)[0].dy,-8),"upward finger pan is reversed");
    output=router.move(sample(2,13,67));check(output&&output->size==1&&near((*output)[0].dx,2)&&near((*output)[0].dy,5),"incremental pan is wrong");
    output=router.up(sample(2,13,67));check(output&&output->size==0,"scroll generated a click");

    output=router.down(sample(3,10,10,true),true);check(output&&output->size==0,"pencil scroll candidate not delayed");
    output=router.move(sample(3,20,11,true));
    check(output&&output->size==2&&(*output)[0].kind==TouchEventKind::pointer_down&&(*output)[1].kind==TouchEventKind::pointer_move&&(*output)[1].sample.pen,"horizontal slider input lost");
    output=router.up(sample(3,20,11,true));check(output&&output->size==1&&(*output)[0].kind==TouchEventKind::pointer_up,"slider release lost");
    output=router.down(sample(4,10,10),true);check(output&&output->size==0,"tap candidate failed");
    output=router.up(sample(4,10,10));check(output&&output->size==2&&(*output)[0].kind==TouchEventKind::pointer_down&&(*output)[1].kind==TouchEventKind::pointer_up,"tap was lost");

    output=router.down(sample(5,0,0),false);check(output&&output->size==1,"first finger down failed");
    output=router.down(sample(6,10,0),false);check(output&&output->size==1&&(*output)[0].kind==TouchEventKind::pointer_cancel,"second finger did not cancel drawing");
    output=router.move(sample(5,10,5),sample(6,30,5));
    check(output&&output->size==2&&(*output)[0].kind==TouchEventKind::pan&&near((*output)[0].dx,15)&&near((*output)[0].dy,5)&&
          (*output)[1].kind==TouchEventKind::pinch&&near((*output)[1].scale,2),"two-finger pan or pinch is incorrect");
    output=router.move(sample(5,15,5),sample(6,25,5));
    check(output&&output->size==1&&(*output)[0].kind==TouchEventKind::pinch&&near((*output)[0].scale,.5),"pinch-in is incorrect");
    output=router.up(sample(6,25,5));check(output&&output->size==0,"gesture end emitted pointer input");
    output=router.move(sample(5,16,5));check(output&&output->size==0,"remaining finger became a stroke");
    output=router.up(sample(5,16,5));check(output&&output->size==0,"remaining finger released a stroke");
    output=router.down(sample(7,0,0),false);check(output&&output->size==1,"router did not reset after gesture");
    output=router.cancel();check(output&&output->size==1&&(*output)[0].kind==TouchEventKind::pointer_cancel,"cancellation lost captured pointer");
    auto invalid=sample(8,0,0);invalid.x=std::numeric_limits<double>::quiet_NaN();
    check(!router.down(invalid,false),"nonfinite input was accepted");
    output=router.down(sample(8,0,0),true);check(output&&output->size==0,"invalid input changed state");
    for(int i=0;i<31;++i){output=router.move(sample(8,.01*i,0));check(output&&output->size==0,"pending sample emitted early");}
    output=router.move(sample(8,.31,0));
    check(output&&output->size==33&&(*output)[0].kind==TouchEventKind::pointer_down&&
          (*output)[32].kind==TouchEventKind::pointer_move,"bounded pending samples were discarded");
    output=router.up(sample(8,.31,0));check(output&&output->size==1,"replayed input did not complete");
    output=router.down(sample(9,0,0),false);check(output&&output->size==1,"three-contact fixture failed");
    output=router.down(sample(10,10,0),false);check(output&&output->size==1,"gesture cancellation failed");
    output=router.down(sample(11,20,0),false);check(output&&output->size==0,"third contact emitted input");
    output=router.move(sample(9,10,0));check(output&&output->size==0,"three-contact gesture was not suppressed");
    (void)router.up(sample(9,10,0));(void)router.up(sample(10,10,0));(void)router.up(sample(11,20,0));
    output=router.down(sample(12,0,0),false);check(output&&output->size==1,"third-contact reset failed");(void)router.cancel();

    output=router.down(sample(13,10,10,true),false);check(output&&output->size==1,"pen press failed");
    output=router.down(sample(14,30,30),false);check(output&&output->size==0,"finger interrupted the pen stroke");
    output=router.move(sample(13,15,12,true),sample(14,30,30));
    check(output&&output->size==1&&(*output)[0].kind==TouchEventKind::pointer_move&&(*output)[0].sample.pen,
          "pen motion was lost while a finger was touching");
    output=router.move(sample(14,32,30),sample(13,15,12,true));check(output&&output->size==0,"palm motion became an input");
    output=router.up(sample(13,15,12,true));check(output&&output->size==1&&(*output)[0].kind==TouchEventKind::pointer_up,"pen release was lost");
    output=router.move(sample(14,35,30));check(output&&output->size==0,"remaining palm became a gesture");
    (void)router.up(sample(14,35,30));

    output=router.down(sample(15,0,0),false);check(output&&output->size==1,"finger press failed");
    output=router.down(sample(16,5,5,true),false);
    check(output&&output->size==2&&(*output)[0].kind==TouchEventKind::pointer_cancel&&
          (*output)[1].kind==TouchEventKind::pointer_down&&(*output)[1].sample.pen,"pen did not take over from the finger");
    output=router.move(sample(16,9,7,true),sample(15,0,0));check(output&&output->size==1&&(*output)[0].sample.pen,"pen takeover lost motion");
    output=router.up(sample(16,9,7,true));check(output&&output->size==1&&(*output)[0].kind==TouchEventKind::pointer_up,"pen takeover lost release");
    output=router.up(sample(15,0,0));check(output&&output->size==0,"old finger produced a click after pen takeover");

    (void)router.down(sample(17,10,10,true),false);(void)router.down(sample(18,30,30),false);
    output=router.cancel(18);check(output&&output->size==0,"canceling a palm interrupted the pen");
    output=router.move(sample(17,12,10,true));check(output&&output->size==1&&(*output)[0].sample.pen,"pen did not survive palm cancellation");
    (void)router.down(sample(19,40,40),false);
    output=router.cancel(17);check(output&&output->size==1&&(*output)[0].kind==TouchEventKind::pointer_cancel,"pen cancellation was lost");
    output=router.move(sample(19,45,40));check(output&&output->size==0,"remaining finger reactivated after pen cancellation");
    (void)router.up(sample(19,45,40));

    auto timed=sample(20,10,40);timed.timestamp=5.;(void)router.down(timed,true);
    check(!router.empty(),"active contact was not reported");
    timed.y=30;timed.timestamp=5.02;output=router.move(timed);
    check(output&&output->size==1&&near((*output)[0].sample.timestamp,5.02)&&near((*output)[0].sample.y,40),
          "scroll sample lost its current time or original target");
    (void)router.up(timed);check(router.empty(),"released contact remained active");
}
