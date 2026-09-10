// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Internal receive-only channelizer shared by the live engine and offline diagnostics.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

namespace ovmesh {
// A bounded cascaded box filter before coarse decimation, then a windowed
// low-pass FIR at four samples/chip. No unbounded CIC integrator states.
class Downconverter {
    using Complex = std::complex<float>;
    static constexpr double pi = std::numbers::pi;
    std::complex<double> osc_{1,0},step_;
    std::array<std::vector<Complex>,3> delay_;
    std::array<Complex,3> sums_{};
    size_t coarse_,pos_=0,tick_=0,history_pos_=0,history_count_=0,phase_=0;
    uint64_t ticks_=0;
    std::array<Complex,97> history_{};
    std::array<float,97> taps_{};
    double first_output_seconds_=0;
public:
    Downconverter(uint32_t rate,uint32_t bandwidth,int64_t offset):coarse_(rate/bandwidth/4) {
        if(!coarse_ || rate%(bandwidth*4)!=0)throw std::runtime_error("Decoder input rate must be an integer multiple of four times its bandwidth");
        // First output follows the 100th coarse-rate sample (97-tap warmup,
        // decimation phase 4). Remove the linear-phase filter group delays.
        first_output_seconds_=(50.5*static_cast<double>(coarse_)+.5)/rate;
        step_=std::polar(1.0,-2*pi*static_cast<double>(offset)/rate);
        for(auto& d:delay_)d.resize(coarse_);
        double total=0,cutoff=0.1375;
        for(size_t n=0;n<taps_.size();++n){double t=static_cast<double>(n)-48;double sinc=t==0?2*cutoff:std::sin(2*pi*cutoff*t)/(pi*t);double window=.54-.46*std::cos(2*pi*static_cast<double>(n)/96);taps_[n]=static_cast<float>(sinc*window);total+=taps_[n];}
        for(auto& tap:taps_)tap=static_cast<float>(tap/total);
    }
    void reset(){osc_={1,0};sums_={};for(auto& d:delay_)std::fill(d.begin(),d.end(),Complex{});history_={};pos_=tick_=history_pos_=history_count_=phase_=0;ticks_=0;}
    double first_output_seconds() const{return first_output_seconds_;}
    void feed(std::span<const Complex> in,std::vector<Complex>& out) {
        out.clear();out.reserve(in.size()/coarse_/4+1);
        for(auto raw:in){Complex x=raw*Complex(osc_);osc_*=step_;if((++ticks_&4095)==0)osc_/=std::abs(osc_);
            for(size_t k=0;k<3;++k){sums_[k]+=x-delay_[k][pos_];delay_[k][pos_]=x;x=sums_[k]/static_cast<float>(coarse_);}
            pos_=(pos_+1)%coarse_;if(++tick_<coarse_)continue;tick_=0;
            history_[history_pos_]=x;history_pos_=(history_pos_+1)%history_.size();history_count_=std::min(history_count_+1,history_.size());
            if(++phase_<4)continue;phase_=0;if(history_count_<history_.size())continue;
            Complex filtered{};size_t p=history_pos_;for(float tap:taps_){p=(p+history_.size()-1)%history_.size();filtered+=history_[p]*tap;}out.push_back(filtered);
        }
    }
};

} // namespace ovmesh
