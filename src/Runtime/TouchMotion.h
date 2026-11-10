#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace ii::runtime::touch {

struct MotionVector final { double x=0, y=0; };
struct MotionPolicy final {
    double sample_window=.12, release_timeout=.10;
    double friction=2., minimum_start_speed=35., stop_speed=6., maximum_speed=8000.;
    double maximum_frame_gap=.30;
};
struct MotionStep final { MotionVector delta, velocity; bool active=false; };

[[nodiscard]] inline bool valid_motion_policy(const MotionPolicy& p) noexcept {
    return std::isfinite(p.sample_window)&&p.sample_window>0&&p.sample_window<=2&&
        std::isfinite(p.release_timeout)&&p.release_timeout>0&&p.release_timeout<=2&&
        std::isfinite(p.friction)&&p.friction>0&&p.friction<=100&&
        std::isfinite(p.minimum_start_speed)&&p.minimum_start_speed>=0&&
        std::isfinite(p.stop_speed)&&p.stop_speed>0&&p.stop_speed<=p.minimum_start_speed&&
        std::isfinite(p.maximum_speed)&&p.maximum_speed>=p.minimum_start_speed&&p.maximum_speed<=1e7&&
        std::isfinite(p.maximum_frame_gap)&&p.maximum_frame_gap>0&&p.maximum_frame_gap<=2;
}

// A bounded, allocation-free history of cumulative displacement and monotonic
// event time. It never changes the source samples or synthesizes input.
class PanVelocity final {
    struct Sample final { double time=0, x=0, y=0; };
    MotionPolicy policy_;
    std::array<Sample,32> samples_{};
    std::size_t head_=0,count_=0;
    [[nodiscard]] const Sample& at(std::size_t i)const noexcept{return samples_[(head_+i)%samples_.size()];}
public:
    explicit PanVelocity(MotionPolicy policy={}):policy_(policy){
        if(!valid_motion_policy(policy))throw std::invalid_argument("Invalid touch motion policy");
    }
    [[nodiscard]] bool begin(double time) noexcept {
        if(!std::isfinite(time)||time<0)return false;
        head_=0;count_=1;samples_[0]={time,0,0};return true;
    }
    [[nodiscard]] bool add(MotionVector delta,double time) noexcept {
        if(!count_||!std::isfinite(time)||time<=at(count_-1).time||
           !std::isfinite(delta.x)||!std::isfinite(delta.y)||std::abs(delta.x)>1e9||std::abs(delta.y)>1e9)return false;
        const auto previous=at(count_-1);Sample next{time,previous.x+delta.x,previous.y+delta.y};
        if(!std::isfinite(next.x)||!std::isfinite(next.y)||std::abs(next.x)>1e12||std::abs(next.y)>1e12)return false;
        if(count_==samples_.size()){head_=(head_+1)%samples_.size();--count_;}
        samples_[(head_+count_)%samples_.size()]=next;++count_;
        while(count_>2&&time-at(1).time>=policy_.sample_window){head_=(head_+1)%samples_.size();--count_;}
        return true;
    }
    [[nodiscard]] MotionVector release(double time)const noexcept {
        if(count_<2||!std::isfinite(time)||time<at(count_-1).time||time-at(count_-1).time>policy_.release_timeout)return {};
        const auto origin=at(count_-1).time;
        double sum_t=0,sum_tt=0,sum_x=0,sum_y=0,sum_tx=0,sum_ty=0;
        for(std::size_t i=0;i<count_;++i){const auto& p=at(i);const double t=p.time-origin;
            sum_t+=t;sum_tt+=t*t;sum_x+=p.x;sum_y+=p.y;sum_tx+=t*p.x;sum_ty+=t*p.y;}
        const auto n=double(count_),denominator=n*sum_tt-sum_t*sum_t;
        if(denominator<=1e-12)return {};
        const double attenuation=std::max(0.,1.-(time-origin)/policy_.release_timeout);
        MotionVector velocity{(n*sum_tx-sum_t*sum_x)/denominator*attenuation,
                              (n*sum_ty-sum_t*sum_y)/denominator*attenuation};
        return std::isfinite(velocity.x)&&std::isfinite(velocity.y)?velocity:MotionVector{};
    }
    void clear()noexcept{head_=count_=0;}
};

// Exact exponential integration is independent of frame frequency. Time is
// supplied by the host's monotonic display clock; this object owns no timer.
class KineticMotion final {
    MotionPolicy policy_;
    MotionVector velocity_{};
    double last_=0;
    bool active_=false;
public:
    explicit KineticMotion(MotionPolicy policy={}):policy_(policy){
        if(!valid_motion_policy(policy))throw std::invalid_argument("Invalid touch motion policy");
    }
    [[nodiscard]] bool start(MotionVector velocity,double time) noexcept {
        if(!std::isfinite(time)||time<0||!std::isfinite(velocity.x)||!std::isfinite(velocity.y))return false;
        const auto speed=std::hypot(velocity.x,velocity.y);
        if(!std::isfinite(speed)||speed<policy_.minimum_start_speed){cancel();return false;}
        const auto scale=std::min(1.,policy_.maximum_speed/speed);
        velocity_={velocity.x*scale,velocity.y*scale};last_=time;active_=true;return true;
    }
    [[nodiscard]] MotionStep advance(double time) noexcept {
        if(!active_||!std::isfinite(time)||time<=last_)return {{},velocity_,active_};
        const auto elapsed=time-last_;last_=time;
        if(elapsed>policy_.maximum_frame_gap){cancel();return {};}
        const auto decay=std::exp(-policy_.friction*elapsed);
        const auto distance=-std::expm1(-policy_.friction*elapsed)/policy_.friction;
        MotionVector delta{velocity_.x*distance,velocity_.y*distance};
        velocity_.x*=decay;velocity_.y*=decay;
        if(std::hypot(velocity_.x,velocity_.y)<=policy_.stop_speed)cancel();
        return {delta,velocity_,active_};
    }
    void block(bool x,bool y)noexcept{
        if(x)velocity_.x=0;if(y)velocity_.y=0;
        if(std::hypot(velocity_.x,velocity_.y)<=policy_.stop_speed)cancel();
    }
    void cancel()noexcept{velocity_={};active_=false;}
    [[nodiscard]] bool active()const noexcept{return active_;}
};
}
