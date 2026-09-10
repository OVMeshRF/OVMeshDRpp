// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Analytic, continuous-time synthetic chirps; no file/device/transmit API.
// This does not use modulate_lora(), but payload symbols deliberately share the
// application's codec. It investigates acquisition/channelization, not an
// independent end-to-end interoperability reference.
#include "ovmesh/phy.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

namespace ovmesh::diagnostics {
class AnalyticChirpSource {
public:
    AnalyticChirpSource(PhyConfig config, std::span<const uint8_t> payload,
                       uint32_t input_rate, double lane_offset_hz,
                       double cfo_hz, double fractional_delay_chips,
                       double sample_clock_ppm, uint8_t transmitted_sync)
        : config_(config), symbols_(encode_lora_symbols(payload,config)), rate_(input_rate),
          carrier_hz_(lane_offset_hz+cfo_hz), delay_chips_(311+fractional_delay_chips),
          clock_scale_(1+sample_clock_ppm*1e-6), transmitted_sync_(transmitted_sync) {
        chips_per_symbol_=static_cast<double>(1U<<config_.spreading_factor);
        payload_start_=20.25*chips_per_symbol_; // 16 up + 2 sync + 2.25 down
        const double end_chips=delay_chips_+payload_start_+
            (static_cast<double>(symbols_.size())+3)*chips_per_symbol_;
        total_=static_cast<uint64_t>(std::ceil(end_chips*rate_/config_.bandwidth_hz/clock_scale_));
    }
    uint64_t sample_count() const {return total_;}
    uint64_t clipped_components() const {return clipped_;}
    void generate(uint64_t first,std::span<std::complex<float>> output) {
        constexpr double pi=std::numbers::pi;
        for(size_t index=0;index<output.size();++index) {
            const double time=static_cast<double>(first+index)/rate_*clock_scale_;
            const double chip=time*config_.bandwidth_hz-delay_chips_;
            bool active=chip>=0,down=false;
            double x=0,symbol=0;
            if(active) {
                if(chip<16*chips_per_symbol_)x=std::fmod(chip,chips_per_symbol_);
                else if(chip<17*chips_per_symbol_){x=chip-16*chips_per_symbol_;symbol=(transmitted_sync_>>4)*8;}
                else if(chip<18*chips_per_symbol_){x=chip-17*chips_per_symbol_;symbol=(transmitted_sync_&15)*8;}
                else if(chip<payload_start_){x=std::fmod(chip-18*chips_per_symbol_,chips_per_symbol_);down=true;}
                else {
                    const double payload_chip=chip-payload_start_;
                    const size_t n=static_cast<size_t>(payload_chip/chips_per_symbol_);
                    if(n>=symbols_.size())active=false;
                    else {symbol=symbols_[n];x=std::fmod(payload_chip,chips_per_symbol_);}
                }
            }
            double real=noise(),imag=noise();
            if(active) {
                x=std::fmod(x+symbol,chips_per_symbol_);
                double phase=pi*(x*x/chips_per_symbol_-x);
                if(down)phase=-phase;
                phase+=2*pi*carrier_hz_*time;
                real+=.22*std::cos(phase);imag+=.22*std::sin(phase);
            }
            output[index]={quantize(real),quantize(imag)};
        }
    }
private:
    double noise() {
        random_^=random_<<13;random_^=random_>>17;random_^=random_<<5;
        return (static_cast<double>(random_&65535)/32768.0-1)*.009;
    }
    float quantize(double sample) {
        long value=std::lround(sample*128);
        if(value< -128 || value>127)++clipped_;
        value=std::clamp(value,-128L,127L);
        return static_cast<float>(value)/128.0f;
    }
    PhyConfig config_;
    std::vector<uint16_t> symbols_;
    uint32_t rate_;
    double carrier_hz_,delay_chips_,clock_scale_,chips_per_symbol_=0,payload_start_=0;
    uint8_t transmitted_sync_;
    uint64_t total_=0,clipped_=0;
    uint32_t random_=17;
};
} // namespace ovmesh::diagnostics
