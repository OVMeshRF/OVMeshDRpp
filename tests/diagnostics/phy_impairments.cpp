// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic impairment investigation only: no files, devices, or RF transmission.
// Generate continuous-time chirps analytically, independently of modulate_lora().
// The payload symbols still use encode_lora_symbols(); this isolates acquisition
// and clock/timing effects, not independent packet-codec interoperability.
#include "ovmesh/phy.hpp"
#include <cmath>
#include <iostream>
#include <numbers>
#include <random>
using C=std::complex<float>;
void run(const char* label,double setting,double delay,double ppm,double cfo,double snr) {
    ovmesh::PhyConfig cfg;
    std::vector<uint8_t> data(43);
    for(int i=0;i<43;++i)data[i]=uint8_t(i*37+9);
    const auto syms=ovmesh::encode_lora_symbols(data,cfg);
    constexpr double n=2048,pi=std::numbers::pi;
    const double payload_start=20.25*n; // firmware's 16 up + 2 sync + 2.25 down
    const size_t len=size_t(delay+payload_start+n*syms.size()+2*n);
    std::vector<C> wave(len);
    std::mt19937 rng(17);
    std::normal_distribution<float> noise(0,float(std::pow(10.,-snr/20)/std::sqrt(2.)));
    for(size_t i=0;i<len;++i) {
        const double t=(double(i)-delay)*(1+ppm*1e-6);
        bool down=false,on=t>=0;
        double x=0,symbol=0;
        if(t<16*n)x=std::fmod(t,n);
        else if(t<17*n){x=t-16*n;symbol=16;}
        else if(t<18*n){x=t-17*n;symbol=88;}
        else if(t<payload_start){x=std::fmod(t-18*n,n);down=true;}
        else {
            const double p=t-payload_start;
            const size_t idx=size_t(p/n);
            if(idx>=syms.size())on=false;
            else {symbol=syms[idx];x=std::fmod(p,n);}
        }
        if(on) {
            x=std::fmod(x+symbol,n);
            double phase=pi*(x*x/n-x);
            if(down)phase=-phase;
            phase+=2*pi*cfo*double(i)/n;
            wave[i]=C(float(std::cos(phase)),float(std::sin(phase)));
        }
        wave[i]+=C(noise(rng),noise(rng));
    }
    ovmesh::LoRaReceiver rx(cfg);
    int count=0;
    rx.feed(wave,[&](ovmesh::PhyFrame&& f){if(f.payload_crc_valid&&f.bytes==data)++count;});
    const auto d=rx.diagnostics();
    std::cout<<label<<'='<<setting<<" valid_payload="<<count<<" preamble="<<d.preamble_candidates
        <<" sync="<<d.sync_matches<<" sync_reject="<<d.sync_rejections
        <<" header_valid="<<d.headers_valid<<" header_failed="<<d.headers_failed
        <<" completed="<<d.completed_frames<<'\n';
}
int main() {
    for(double snr:{30.,-5.,-8.,-10.,-12.,-15.})run("SNR_dB",snr,311,0,2.23,snr);
    for(double frac:{0.,.125,.25,.375,.5,.625,.75,.875})run("fraction_chip",frac,311+frac,0,2.23,30);
    for(double ppm:{-20.,-10.,-5.,5.,10.,20.})run("clock_ppm",ppm,311.25,ppm,2.23,30);
}
