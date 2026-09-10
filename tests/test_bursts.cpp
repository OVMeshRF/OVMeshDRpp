// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/bursts.hpp"
#include "../src/spectrum.hpp"
#include <cmath>
#include <complex>
#include <iostream>
#include <numbers>
#include <stdexcept>

using namespace ovmesh;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
bool near(double a,double b) {return std::abs(a-b)<1e-8;}
SpectrumTile tile(size_t id, const std::vector<size_t>& bins) {
    SpectrumTile t; t.id=id+1; t.frame_count=80; t.fft_size=4096;
    t.first_sample=id*80*4096; t.end_sample=t.first_sample+80*4096;
    t.elapsed_start_seconds=double(id)*.020; t.elapsed_end_seconds=double(id+1)*.020;
    t.first_center_hz=905000000; t.bin_width_hz=4000;
    t.mean_dbfs.assign(128,-80); t.peak_dbfs.assign(128,-40); t.activity.resize(16*80);
    for(size_t f=0;f<80;++f) for(auto b:bins) t.activity[f*16+b/8]|=uint8_t(1u<<(b%8));
    return t;
}
void association() {
    std::vector<SpectrumBurst> out; auto emit=[&](SpectrumBurst b){out.push_back(b);};
    BurstGrouper group(905256000);
    group.consume(tile(0,{10,11,90,91,64}),emit);
    group.consume(tile(1,{}),emit); // Quiet time is observed, never added to active time.
    group.consume(tile(2,{10,11,90,91,64}),emit);
    group.finish(emit);
    require(out.size()==3,"Separated emitters and receiver center remain distinct");
    for(const auto& b:out) require(near(b.active_seconds,.040)&&near(b.elapsed_end_seconds-b.elapsed_start_seconds,.060),"Quiet grouping gaps are excluded from active duration");
    require(out[1].center_region || out[2].center_region,"Center group labeled");
    out.clear(); BurstGrouper gap(905256000);
    gap.consume(tile(0,{10}),emit);gap.consume(tile(2,{10}),emit);gap.finish(emit);
    require(out.size()==2 && out[0].limited,"No grouping across unobserved missing tile");
    out.clear(); BurstGrouper separate(905256000);
    for(size_t i=0;i<8;++i) separate.consume(tile(i,i==0||i==7?std::vector<size_t>{10}:std::vector<size_t>{}),emit);
    separate.finish(emit);require(out.size()==2,"Separated bursts expire across measured quiet intervals");
    out.clear(); BurstGrouper clip(905256000); const auto original=tile(0,{10,90});auto copy=original;
    clip.consume(copy,emit,0,40,.005,.015);clip.finish(emit);
    require(out.size()==1 && near(out[0].active_seconds,.010) && copy.activity==original.activity,"Time/frequency query clipping conserves source masks");
    out.clear(); BurstGrouper ambiguity(905256000);
    ambiguity.consume(tile(0,{10,20}),emit);
    ambiguity.consume(tile(1,{10,11,12,13,14,15,16,17,18,19,20}),emit);ambiguity.finish(emit);
    require(out.size()==3,"Ambiguous bridge does not merge existing independent tracks");
    for(const auto& b:out)require(b.ambiguous,"All ambiguous alternatives flagged");
}
void chirp_fixture(double bandwidth, size_t symbol) {
    // Independent analytic RF source: eight repeated 8 ms 250 kHz sweeps,
    // with periodic one-frame fades. No application modem/encoder is used.
    constexpr uint32_t rate=16384000; constexpr size_t frame=4096;
    SpectrumProcessor spectrum(907500000,rate,10000000,-55);
    BurstGrouper group(907500000);
    std::vector<SpectrumBurst> bursts; size_t fragments=0; double phase=0;
    const auto emit=[&](SpectrumBurst b){bursts.push_back(b);};
    const auto on_tile=[&](SpectrumTile t){group.consume(t,emit);};
    const auto on_event=[&](SpectrumEvent){++fragments;};
    std::vector<std::complex<float>> samples(frame);
    for(size_t offset=0;offset<symbol*8;++offset) {
        const double frequency=-625000-bandwidth/2+bandwidth*double(offset%symbol)/double(symbol);
        phase+=2*std::numbers::pi*frequency/rate;
        const float amplitude=((offset/frame)%13==12)?0.0f:.5f;
        samples[offset%frame]=std::polar(amplitude,float(std::remainder(phase,2*std::numbers::pi)));
        if(offset%frame==frame-1)spectrum.feed(samples,offset+1-frame,on_tile,on_event);
    }
    spectrum.finish(on_tile,on_event);group.finish(emit);
    require(fragments>8,"Independent chirps reproduce many raw fragments");
    require(bursts.size()==1,"Repeated sweeping signal with brief fades forms one candidate burst");
    require(bursts[0].upper_hz-bursts[0].lower_hz>=bandwidth*.95 && bursts[0].upper_hz-bursts[0].lower_hz<bandwidth*1.15,"Envelope reflects measured sweep rather than tiny instantaneous fragments");
    const double elapsed=double(symbol*8)/rate;
    require(bursts[0].active_seconds<elapsed && bursts[0].active_seconds>elapsed*.75,"Fades remain absent from active duration");
    std::cout<<"Independent chirp: "<<fragments<<" fragments -> "<<bursts.size()<<" candidate, envelope "<<(bursts[0].upper_hz-bursts[0].lower_hz)/1000<<" kHz\n";
}
void brief_and_bounded() {
    std::vector<SpectrumBurst> out; auto emit=[&](SpectrumBurst b){out.push_back(b);};
    BurstGrouper brief(905256000);auto t=tile(0,{});t.activity[1]=4;const auto mask=t.activity;
    brief.consume(t,emit);brief.finish(emit);
    require(out.empty()&&t.activity==mask,"One-frame energy remains in evidence without claiming a candidate band");
    BurstGrouper continuous(905256000);
    for(size_t i=0;i<510;++i)continuous.consume(tile(i,{10}),emit);
    continuous.finish(emit);require(out.size()==2&&out[0].limited,"Persistent activity produces bounded segments");
    double active=0;for(const auto& b:out)active+=b.active_seconds;
    require(near(active,10.2),"Segmentation preserves active duration");
}
void split_sweep() {
    for(bool simultaneous:{false,true}) {
        std::vector<SpectrumBurst> out;auto emit=[&](SpectrumBurst b){out.push_back(b);};
        BurstGrouper group(905256000);
        group.consume(tile(0,{10,11,12,13,14,15,16,17,18,19,20}),emit);
        auto pieces=tile(1,{});
        for(size_t f=0;f<80;++f){
            if(simultaneous||f<40)pieces.activity[f*16+10/8]|=uint8_t(1u<<(10%8));
            if(simultaneous||f>=40)pieces.activity[f*16+20/8]|=uint8_t(1u<<(20%8));
        }
        group.consume(pieces,emit);group.finish(emit);
        if(simultaneous)require(out.size()==3,"Separated simultaneous signals are not absorbed as a frequency sweep");
        else require(out.size()==1&&near(out[0].active_seconds,.040)&&out[0].ambiguous,
            "Disjoint-time pieces inside an established band preserve one candidate with exact union duration");
    }
}
int main(){try{association();brief_and_bounded();split_sweep();chirp_fixture(250000,131072);chirp_fixture(500000,65536);std::cout<<"Burst grouping checks passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
