// SPDX-License-Identifier: GPL-3.0-or-later
// Manual offline investigation, NOT a passing acceptance-test assertion.
// Analytic 8 MS/s RF + signed 8-bit quantization -> the actual live engine's
// Downconverter -> LoRaReceiver. Outputs only categorical aggregate counts.
// Compile from repository root:
// clang++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
//   -Iinclude -Isrc tests/diagnostics/channelizer_impairments.cpp \
//   src/phy.cpp src/phy_codec.cpp -o build/phy-standalone/channelizer_impairments
#include "channelizer.hpp"
#include "synthetic_chirps.hpp"
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {
struct Profile {const char* name;uint64_t frequency;uint32_t bandwidth;uint8_t cr;};
struct Result {uint64_t matching_payloads=0;bool wrong_sync=false;};
Result run(Profile profile,std::string_view sweep,double cfo,double fraction,double ppm,uint8_t sync=0x2b) {
    constexpr uint32_t rate=8000000;
    constexpr uint64_t center=907500000;
    ovmesh::PhyConfig cfg;cfg.bandwidth_hz=profile.bandwidth;cfg.spreading_factor=11;cfg.coding_rate=profile.cr;
    std::array<uint8_t,43> payload{};
    for(size_t i=0;i<payload.size();++i)payload[i]=static_cast<uint8_t>(i*37+9);
    const auto offset=static_cast<int64_t>(profile.frequency)-static_cast<int64_t>(center);
    ovmesh::diagnostics::AnalyticChirpSource source(cfg,payload,rate,static_cast<double>(offset),cfo,fraction,ppm,sync);
    ovmesh::Downconverter converter(rate,profile.bandwidth,offset);
    ovmesh::LoRaReceiver receiver(cfg);
    std::array<std::complex<float>,8192> input{};
    std::vector<std::complex<float>> output;
    uint64_t payload_matches=0,crc_valid=0,converted=0;
    for(uint64_t first=0;first<source.sample_count();) {
        const size_t count=static_cast<size_t>(std::min<uint64_t>(input.size(),source.sample_count()-first));
        const std::span<std::complex<float>> chunk(input.data(),count);
        source.generate(first,chunk);converter.feed(chunk,output);converted+=output.size();
        receiver.feed(output,[&](ovmesh::PhyFrame&& frame) {
            if(frame.payload_crc_valid) {
                ++crc_valid;
                if(frame.bytes.size()==payload.size() && std::equal(frame.bytes.begin(),frame.bytes.end(),payload.begin()))++payload_matches;
            }
            std::fill(frame.bytes.begin(),frame.bytes.end(),uint8_t{});
        });
        std::fill(input.begin(),input.end(),std::complex<float>{});
        std::fill(output.begin(),output.end(),std::complex<float>{});first+=count;
    }
    const auto d=receiver.diagnostics();
    std::cout<<"profile="<<profile.name<<" sweep="<<sweep<<" nominal_center_hz="<<center
        <<" nominal_lane_hz="<<profile.frequency<<" sample_rate="<<rate<<" quantization_bits=8"
        <<" bandwidth_hz="<<profile.bandwidth<<" sf=11 cr=4/"<<unsigned(profile.cr)
        <<" cfo_hz="<<cfo<<" fraction_chip="<<fraction<<" sample_clock_ppm="<<ppm
        <<" tx_sync=0x"<<std::hex<<unsigned(sync)<<std::dec<<" expected="<<(sync==cfg.sync_word?"one_matching_frame":"no_matching_frame")
        <<" matching_payloads="<<payload_matches<<" crc_valid_frames="<<crc_valid
        <<" preamble="<<d.preamble_candidates<<" sync_matches="<<d.sync_matches
        <<" sync_rejections="<<d.sync_rejections<<" sync_low_ratio="<<d.sync_low_ratio
        <<" sync_timeout="<<d.sync_timeout<<" sync_first_mismatch="<<d.sync_first_mismatch
        <<" sync_second_mismatch="<<d.sync_second_mismatch<<" headers_valid="<<d.headers_valid
        <<" headers_failed="<<d.headers_failed<<" completed_frames="<<d.completed_frames
        <<" converted_samples="<<converted<<" first_output_seconds="<<converter.first_output_seconds()
        <<" clipped_components="<<source.clipped_components()<<'\n'<<std::flush;
    return {payload_matches,sync!=cfg.sync_word};
}
}
int main() {
    const auto started=std::chrono::steady_clock::now();
    uint64_t positive=0,decoded=0,negative=0,unexpected_negative_decodes=0;
    auto collect=[&](Result r){if(r.wrong_sync){++negative;if(r.matching_payloads)++unexpected_negative_decodes;}
        else {++positive;if(r.matching_payloads==1)++decoded;}};
    for(const Profile p:std::array<Profile,2>{{{"LongFast",906875000,250000,5},{"LongTurbo",908750000,500000,8}}}) {
        for(double cfo:{0.,-900.,900.,-5000.,5000.,-20000.,20000.,-50000.,50000.})collect(run(p,"carrier",cfo,.25,0));
        for(double fraction:{0.,.125,.375,.5,.625,.75,.875})collect(run(p,"timing",900,fraction,0));
        for(double ppm:{-20.,20.})collect(run(p,"clock",900,.25,ppm));
        for(uint8_t sync:{uint8_t{0x12},uint8_t{0x34}})collect(run(p,"wrong_sync",0,.25,0,sync));
    }
    std::cout<<"diagnostic_complete=1 acceptance_pass_claim=0 positive_cases="<<positive<<" decoded_once="<<decoded
        <<" not_decoded_once="<<(positive-decoded)<<" wrong_sync_controls="<<negative
        <<" unexpectedly_decoded_controls="<<unexpected_negative_decodes
        <<" wall_seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()<<'\n';
}
