// SPDX-License-Identifier: GPL-3.0-or-later
// Independent analytic chirp tails. No packet bytes, FEC, crypto, hardware or
// application transmitter/modulator. These exercise occupied post-SFD windows.
#include "lora_discovery.hpp"
#include "lora_discovery_fixtures.hpp"
#include <array>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {
namespace f=lora_discovery_fixtures;
constexpr double fs=2'000'000,receiver_center=907'500'000;
constexpr uint64_t origin=30'000'017;
constexpr std::array<unsigned,12> symbols{0,4,1023,2047,341,1501,29,1731,887,1183,13,2003};
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
std::vector<ovmesh::LoRaDiscovery> detect(std::span<const f::Complex> input) {
    ovmesh::LoRaPreambleDiscovery detector(receiver_center);std::vector<ovmesh::LoRaDiscovery> found;
    for(size_t i=0;i<input.size();i+=4093)
        detector.feed(input.subspan(i,std::min(size_t{4093},input.size()-i)),origin+i,
            [&](const auto& observation){found.push_back(observation);});
    return found;
}
void verify_transition_equation(const f::PreambleSpec& noisy) {
    auto spec=noisy;spec.noise_rms=0;
    const auto packet=f::make_preamble_with_chirp_tail(spec,symbols);
    const double boundary=packet.truth.sfd_end_sample;
    const auto first=static_cast<size_t>(std::ceil(boundary));
    require(first>0&&first<packet.samples.size(),"valid fractional transition indices");
    const double before=(boundary-static_cast<double>(first-1))/fs;
    const double after=(static_cast<double>(first)-boundary)/fs;
    const double slope=spec.bandwidth_hz/packet.truth.symbol_seconds;
    // Integrate the instantaneous frequencies independently over the two pieces:
    // final quarter-downchirp segment then the first data-like upchirp segment.
    const double cycles=packet.truth.received_center_offset_hz/fs+
        .25*spec.bandwidth_hz*before+.5*slope*before*before+
        spec.bandwidth_hz*(static_cast<double>(symbols[0])/(1u<<spec.spreading_factor)-.5)*after+
        .5*slope*after*after;
    const auto expected=std::polar(1.,2*std::numbers::pi*cycles);
    const auto measured=std::complex<double>(packet.samples[first])*std::conj(std::complex<double>(packet.samples[first-1]))/(spec.amplitude*spec.amplitude);
    require(std::abs(expected-measured)<2e-5,"post-SFD analytic phase continuity");
    require(packet.truth.chirp_tail_symbols==symbols.size()&&packet.truth.chirp_tail_end_sample>packet.truth.sfd_end_sample,"tail truth endpoints");
    for(size_t i=first;i<packet.truth.end_sample;++i)
        require(std::abs(std::abs(packet.samples[i])-spec.amplitude)<1e-6,"continuous occupied tail amplitude");
    auto old_spec=spec;old_spec.trailing_samples=0;const auto old=f::make_preamble(old_spec);
    require(std::equal(old.samples.begin(),old.samples.end(),packet.samples.begin()),"tail must not change preceding preamble samples");
}
struct Suite {unsigned passed=0,failed=0;};
void check(Suite& suite,const f::PreambleSpec& spec,unsigned phase,const std::string& condition) {
    try {
        const auto fixture=condition.starts_with("quiet_")?f::make_preamble(spec):f::make_preamble_with_chirp_tail(spec,symbols);
        auto input=fixture.samples;
        if(condition=="tail_sparse_cw")f::add_cw(input,fs,-713271.,.3);
        if(condition.ends_with("adjacent_cw"))f::add_cw(input,fs,fixture.truth.received_center_offset_hz+spec.bandwidth_hz/2.+17321.,.3);
        const auto found=detect(input);
        std::ostringstream description;description<<std::setprecision(10)<<condition<<" BW="<<spec.bandwidth_hz<<" SF11 phase="<<phase<<"/16 detections="<<found.size();
        bool correct=found.size()==1;
        for(const auto& item:found) {
            const double error=item.center_hz-receiver_center-fixture.truth.received_center_offset_hz;
            const double timing_error=item.delimiter_sample-static_cast<double>(origin)-fixture.truth.sync_end_sample;
            const double observed_start=item.first_observed_upchirp_sample-static_cast<double>(origin);
            description<<" [BW="<<item.bandwidth_hz<<" SF="<<item.spreading_factor<<" frequency_error="<<error<<" delimiter_error_samples="<<timing_error<<" observed_up_start="<<observed_start<<']';
            correct=correct&&item.bandwidth_hz==spec.bandwidth_hz&&item.spreading_factor==spec.spreading_factor&&std::abs(error)<fs/fixture.truth.symbol_samples;
            // Delimiter is its inferred beginning, not the end of the 2.25
            // downchirps. Keep its timing accurate for acquisition-time GPS.
            correct=correct&&std::abs(timing_error)<=2*fs/spec.bandwidth_hz;
            const double observed_up=item.first_observed_upchirp_sample-static_cast<double>(origin);
            description<<" first_observed_after_start_samples="<<observed_up-fixture.truth.start_sample;
            correct=correct&&observed_up>=fixture.truth.start_sample-2*fs/spec.bandwidth_hz&&
                observed_up<fixture.truth.preamble_end_sample;
            correct=correct&&observed_start>=fixture.truth.start_sample-2*fs/spec.bandwidth_hz
                &&item.first_observed_upchirp_sample<item.delimiter_sample;
        }
        if(correct){++suite.passed;std::cout<<"PASS ";}else{++suite.failed;std::cout<<"FAIL ";}
        std::cout<<description.str()<<std::endl;
    } catch(const std::exception& e){++suite.failed;std::cout<<"FAIL "<<condition<<" BW="<<spec.bandwidth_hz<<" phase="<<phase<<": "<<e.what()<<std::endl;}
}
}
int main(int argc,char** argv) {
    try {
        const std::string selection=argc>1?argv[1]:"all";
        if(selection!="all"&&selection!="tail"&&selection!="quiet"&&selection!="controls"&&selection!="fixture")throw std::runtime_error("Choose all, tail, quiet, controls, or fixture");
        Suite suite;
        for(uint32_t bandwidth:{250000u,500000u})for(unsigned phase=0;phase<16;++phase) {
            f::PreambleSpec spec;spec.bandwidth_hz=bandwidth;spec.spreading_factor=11;spec.preamble_symbols=16;spec.sync_word=0x2b;
            spec.center_offset_hz=bandwidth==250000?-311347.:217813.;spec.cfo_hz=phase%2?619.375:-1319.625;
            spec.noise_rms=.02;spec.amplitude=.2;spec.seed=291739+phase*107+bandwidth;
            spec.initial_phase_radians=.191*phase;
            const double n=fs*(1u<<spec.spreading_factor)/bandwidth;
            const double start=127.371+static_cast<double>(phase)*n/16.;
            spec.leading_samples=static_cast<size_t>(start);spec.fractional_start_samples=start-static_cast<double>(spec.leading_samples);
            spec.trailing_samples=static_cast<size_t>(2*n);
            verify_transition_equation(spec);
            if(selection=="all"||selection=="quiet")check(suite,spec,phase,"quiet_after_sfd");
            if(selection=="all"||selection=="controls")check(suite,spec,phase,"quiet_adjacent_cw");
            if(selection=="all"||selection=="tail")for(const auto* kind:{"tail","tail_sparse_cw","tail_adjacent_cw"})check(suite,spec,phase,kind);
        }
        std::cout<<"Independent payload-transition checks: "<<suite.passed<<" passed, "<<suite.failed<<" failed; 32 analytic boundary/amplitude checks passed\n";
        return suite.failed?1:0;
    }catch(const std::exception& e){std::cerr<<"Payload-transition fixture failed: "<<e.what()<<'\n';return 1;}
}
