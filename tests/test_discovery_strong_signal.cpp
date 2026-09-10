// SPDX-License-Identifier: GPL-3.0-or-later
// Independent analytic strong-source controls and a real weak companion.
// Low whole-subband fit fractions cannot by themselves reject coherent signals.
#include "discovery_channelizer.hpp"
#include "discovery_observations.hpp"
#include "lora_discovery.hpp"
#include "lora_discovery_fixtures.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace f=lora_discovery_fixtures;
using Bank=ovmesh::DiscoveryChannelizer;
constexpr double receiver_center=907500000, native_center=906500000;
constexpr double strong_center=906877316.119, companion_center=906209439;
constexpr std::uint64_t input_origin=99991;
void require(bool v,const char* message){if(!v)throw std::runtime_error(message);}
struct Detection {
    ovmesh::LoRaDiscovery value;
    std::size_t subband;
    double subband_center;
    double delimiter_input_sample;
    double observed_up_input_sample;
};

struct PipelineResult {
    std::vector<Detection> raw;
    std::vector<ovmesh::DiscoveryObservation> consolidated;
};

struct Stream {
    std::unique_ptr<ovmesh::LoRaPreambleDiscovery> detector;
    std::uint64_t first_input = 0, delivered = 0;
    std::uint32_t stride = 0;
    bool started = false;
};

PipelineResult run_pipeline(std::uint32_t input_rate, const f::Fixture& fixture) {
    Bank bank(input_rate, receiver_center, receiver_center - 2500000,
              receiver_center + 2500000);
    std::vector<Stream> streams;
    for (const auto& subband : bank.subbands()) {
        Stream stream;
        stream.detector = std::make_unique<ovmesh::LoRaPreambleDiscovery>(subband.center_hz);
        streams.push_back(std::move(stream));
    }
    PipelineResult result;
    ovmesh::DiscoveryObservations observations(input_rate, streams.size());
    auto deliver = [&](const Bank::Samples& samples) {
        auto& stream = streams.at(samples.subband_index);
        require(samples.input_sample_stride == input_rate / Bank::output_sample_rate,
                "Channelizer delivered the wrong sample stride");
        if (!stream.started) {
            stream.first_input = samples.first_input_sample;
            stream.stride = samples.input_sample_stride;
            stream.started = true;
        }
        require(samples.first_input_sample == stream.first_input + stream.delivered * stream.stride,
                "PFB output timestamp is discontinuous across irregular input chunks");
        stream.detector->feed(samples.values, stream.delivered, [&](const auto& discovery) {
            require(std::isfinite(discovery.delimiter_sample) &&
                    std::isfinite(discovery.first_observed_upchirp_sample),
                    "Detector returned a non-finite sample coordinate");
            // Keep the original FIR-centered anchor intact. Integer division
            // by stride here loses origin phase before fractional timing fits.
            result.raw.push_back({discovery, samples.subband_index, bank.subbands()[samples.subband_index].center_hz,
                static_cast<double>(stream.first_input) + discovery.delimiter_sample * stream.stride,
                static_cast<double>(stream.first_input) + discovery.first_observed_upchirp_sample * stream.stride});
            observations.observe(discovery, samples.subband_index, stream.first_input, stream.stride);
        });
        stream.delivered += samples.values.size();
    };
    constexpr std::size_t block = 4093;
    for (std::size_t at = 0; at < fixture.samples.size(); at += block)
        bank.feed(std::span(fixture.samples).subspan(at, std::min(block, fixture.samples.size() - at)),
                  input_origin + at, deliver);
    for (const auto& stream : streams) {
        require(stream.started && stream.detector->stats().samples == stream.delivered,
                "Channelized exposure was not fully delivered to the detector");
        require(stream.first_input % stream.stride != 0,
                "Timestamp fixture must exercise a non-stride-aligned input anchor");
    }
    require(observations.eviction_count() == 0, "Short fixture unexpectedly evicted discovery observations");
    result.consolidated = observations.observations();
    return result;
}


struct Case {
    const char* name;
    bool pfb=false, quantized=false, noise=false, companion=false, shifted_start=false;
    double clock_ppm=0;
};
struct Source {
    f::Fixture fixture;
    double received_strong=0, received_companion=0;
};
Source signal(const Case& test) {
    const std::uint32_t fs=test.pfb?16000000:2000000;
    const double center=test.pfb?receiver_center:native_center;
    const double clock_scale=1+test.clock_ppm*1e-6;
    f::PreambleSpec spec;
    spec.sample_rate_hz=fs*clock_scale;spec.center_offset_hz=strong_center-center;
    spec.bandwidth_hz=250000;spec.spreading_factor=11;spec.preamble_symbols=16;spec.sync_word=0x2b;
    spec.leading_samples=test.shifted_start?25051:static_cast<size_t>(.003*fs);
    spec.fractional_start_samples=.371;spec.trailing_samples=static_cast<size_t>(.02*fs);
    spec.noise_rms=0;spec.amplitude=.65;
    auto fixture=f::make_preamble(spec);
    if(test.companion) {
        // A separate mathematical source at -50.24 dB relative power, with
        // the SAME symbol clock/delimiter. Synchrony is not proof of an image.
        spec.center_offset_hz=companion_center-center;spec.amplitude=.002;
        spec.initial_phase_radians=.719;const auto weak=f::make_preamble(spec);
        require(weak.samples.size()==fixture.samples.size(),"Aligned companion duration");
        for(size_t i=0;i<fixture.samples.size();++i)fixture.samples[i]+=weak.samples[i];
    }
    if(test.noise)f::add_noise(fixture.samples,.0005,972391);
    if(test.quantized)for(auto& x:fixture.samples)
        x={static_cast<float>(static_cast<int8_t>(std::clamp(x.real(),-.99f,.99f)*127))/128.f,
           static_cast<float>(static_cast<int8_t>(std::clamp(x.imag(),-.99f,.99f)*127))/128.f};
    // Detector frequency axes use the assumed sample rate. Keep physical clock
    // mismatch in the truth rather than compare with an unscaled nominal RF.
    return {std::move(fixture),center+(strong_center-center)/clock_scale,
            center+(companion_center-center)/clock_scale};
}
void check(const Case& test) {
    const auto source=signal(test);const auto& fixture=source.fixture;
    const std::uint32_t fs=test.pfb?16000000:2000000;
    std::vector<Detection> raw;size_t consolidated=0;
    if(test.pfb) {
        auto result=run_pipeline(fs,fixture);raw=std::move(result.raw);
        consolidated=result.consolidated.size();
    } else {
        ovmesh::LoRaPreambleDiscovery detector(native_center);
        for(size_t i=0;i<fixture.samples.size();i+=4093)
            detector.feed(std::span(fixture.samples).subspan(i,std::min(size_t{4093},fixture.samples.size()-i)),i,
                [&](const auto& d){raw.push_back({d,0,native_center,d.delimiter_sample,d.first_observed_upchirp_sample});});
        consolidated=raw.size();
    }
    size_t strong=0,weak=0,extras=0;double smallest_weak_down=1;
    const double expected_delimiter=fixture.truth.sync_end_sample+(test.pfb?input_origin:0);
    const double expected_start=fixture.truth.start_sample+(test.pfb?input_origin:0);
    for(const auto& d:raw) {
        const auto& v=d.value;
        const bool profile=v.bandwidth_hz==250000&&v.spreading_factor==11;
        const bool sm=profile&&std::abs(v.center_hz-source.received_strong)<250000./2048;
        const bool wm=test.companion&&profile&&std::abs(v.center_hz-source.received_companion)<250000./2048;
        if(sm)++strong;else if(wm){++weak;smallest_weak_down=std::min(smallest_weak_down,v.down_match);}else ++extras;
        std::cout<<std::setprecision(12)<<test.name<<" subband="<<d.subband<<" subband_center="<<d.subband_center
                 <<" received_center="<<v.center_hz<<" BW="<<v.bandwidth_hz<<" SF="<<v.spreading_factor
                 <<" delimiter_sample="<<d.delimiter_input_sample<<" up="<<v.up_match<<" down="<<v.down_match
                 <<" truth="<<(sm?"strong":wm?"weak":"FALSE_EXTRA")<<'\n';
        require(std::abs(d.delimiter_input_sample-expected_delimiter)<2.*fs/250000,
                "Known source delimiter exceeds two input-rate chips");
        require(d.observed_up_input_sample>=expected_start-2.*fs/250000&&
                d.observed_up_input_sample<expected_delimiter,"Preamble metadata outside known source");
    }
    require(strong>0,"Independent strong source was not found");
    require(extras==0,"Known one/two-source fixture produced an unsupported extra observation");
    require(!test.companion||weak>0,"Real weak companion discarded beside a strong source");
    require(!test.companion||smallest_weak_down<.00002,
            "Companion must exercise very small whole-subband match fractions");
    require(consolidated==(test.companion?2u:1u),"Wrong source count after overlapping-subband association");
    std::cout<<"PASS "<<test.name<<" strong="<<strong<<" weak="<<weak<<" extras="<<extras
             <<" consolidated="<<consolidated<<'\n';
}
}
int main() {
    try {
        for(const auto& test:{Case{"native_single"},Case{"native_q8_noise",false,true,true},
            Case{"pfb_single",true},Case{"pfb_q8_noise",true,true,true},
            Case{"pfb_shifted_q8_noise",true,true,true,false,true},
            Case{"pfb_clock20ppm_q8_noise",true,true,true,false,true,20},
            Case{"native_weak_noise",false,false,true,true},
            Case{"pfb_weak_noise",true,false,true,true},Case{"pfb_weak_q8_noise",true,true,true,true}})check(test);
        std::cout<<"Independent strong-source and weak-companion controls passed (9 cases)\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
