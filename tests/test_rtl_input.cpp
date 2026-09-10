// SPDX-License-Identifier: GPL-3.0-or-later
// Analytic samples and a fake async reader only; never opens USB hardware.
#include "ovmesh/engine.hpp"
#include "rtl_input.hpp"
#include "discovery_worker.hpp"
#include "lora_discovery_fixtures.hpp"
#include <array>
#include <cmath>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
void sample_checks() {
    std::complex<double> sum{};
    for(unsigned value=0;value<256;++value) {
        const auto sample=ovmesh::rtl_iq_sample(static_cast<uint8_t>(value),static_cast<uint8_t>(255-value));
        require(sample.real()==-sample.imag(),"Unsigned RTL IQ must be symmetric about half an ADC step");
        require(std::isfinite(sample.real())&&std::abs(sample.real())<1,"RTL IQ must remain finite and below full scale");
        sum+=sample;
    }
    require(sum==std::complex<double>{},"Uniform offset-binary samples must not introduce a DC bias");
    require(ovmesh::rtl_iq_sample(0,255)==std::complex<float>{-127.5f/128,127.5f/128},"RTL endpoints must not wrap as signed int8 values");
    require(ovmesh::rtl_iq_sample(127,128)==std::complex<float>{-.5f/128,.5f/128},"RTL midpoint polarity must be correct");
    const std::array<int,6> gains{0,90,140,280,297,496};
    require(ovmesh::nearest_rtl_gain(gains,280)==280&&ovmesh::nearest_rtl_gain(gains,282)==280,
        "Manual tuner gain must select a reported supported setting");
    require(ovmesh::nearest_rtl_gain(gains,45)==0,"Gain ties must choose the lower gain deterministically");
    require(ovmesh::nearest_rtl_gain(gains,std::numeric_limits<int>::min())==0&&
        ovmesh::nearest_rtl_gain(gains,std::numeric_limits<int>::max())==496,"Extreme gain requests must not overflow comparison");
    bool rejected=false;try { (void)ovmesh::nearest_rtl_gain({},100); }catch(const std::runtime_error&) { rejected=true; }
    require(rejected,"An absent manual gain table must not become a guessed gain");
}
void lifecycle_checks() {
    ovmesh::RtlAsyncPump pump;
    // Model librtlsdr initializing its cancel state after the parent has already
    // requested Stop. The first cancellation is intentionally lost.
    std::atomic<bool> reader_ready=false,reader_cancelled=false,reader_finished=false;
    std::atomic<unsigned> cancellations=0;
    pump.start([&] {
        while(cancellations.load()==0)std::this_thread::yield();
        reader_cancelled=false;reader_ready=true;
        while(!reader_cancelled.load())std::this_thread::yield();
        reader_finished=true;
    },[&] { ++cancellations;if(reader_ready.load())reader_cancelled=true; });
    pump.stop();
    require(reader_finished.load()&&cancellations>=2,"Stop must cancel through the async initialization race and join before close");
    pump.stop();
    for(unsigned n=0;n<25;++n) {
        std::atomic<bool> done=false;
        pump.start([&] { done=true; },[] {});
        pump.stop();
        require(done,"Start after a previous stop must create and join a new input thread");
    }
}
void waveform_checks() {
    constexpr double center=907500000;
    for(const auto bandwidth : {125000U,250000U,500000U}) {
        lora_discovery_fixtures::PreambleSpec spec;
        spec.sample_rate_hz=2000000;spec.bandwidth_hz=bandwidth;
        spec.spreading_factor=8;spec.center_offset_hz=312731;spec.cfo_hz=917.25;
        spec.leading_samples=751;spec.fractional_start_samples=.375;spec.noise_rms=.001;
        auto fixture=lora_discovery_fixtures::make_preamble(spec);
        for(auto& sample:fixture.samples) {
            const auto quantize=[](float value) {
                return static_cast<uint8_t>(std::clamp(std::lround(value*128+127.5f),0L,255L));
            };
            sample=ovmesh::rtl_iq_sample(quantize(sample.real()),quantize(sample.imag()));
        }
        ovmesh::DiscoveryWorker worker(2000000,center,center-750000,center+750000);
        for(size_t offset=0;offset<fixture.samples.size();offset+=ovmesh::DiscoveryWorker::maximum_input_block) {
            const auto count=std::min(ovmesh::DiscoveryWorker::maximum_input_block,fixture.samples.size()-offset);
            require(worker.submit(std::span(fixture.samples).subspan(offset,count),offset),"Bounded RTL waveform fixture was rejected");
        }
        worker.finish();
        const auto status=worker.snapshot();
        require(!status.failed&&status.rejected_input_samples==0&&
            status.channelized_input_samples==fixture.samples.size(),"RTL waveform discovery lost accepted sample coverage");
        const auto found=worker.take_results();
        require(std::any_of(found.begin(),found.end(),[&](const auto& result) {
            return result.subband_index==0&&result.input_sample_stride==1&&result.complete_in_requested_range&&
                result.waveform.bandwidth_hz==bandwidth&&result.waveform.spreading_factor==8&&
                std::abs(result.waveform.center_hz-(center+spec.center_offset_hz+spec.cfo_hz))<4000;
        }),"Unsigned RTL IQ must discover an off-center 125/250/500 kHz chirp through the low-rate worker");
    }
}
void config_checks() {
    ovmesh::Engine engine;ovmesh::ReceiverConfig c;std::string error;
    c.synthetic=false;c.hardware_receiver=ovmesh::HardwareReceiver::RtlSdr;
    c.sample_rate=2000000;c.survey_span_hz=1500000;c.lanes.clear();c.discover_lora=true;
    require(std::string(ovmesh::receiver_source_name(c))=="RTL-SDR","Hardware source label must identify RTL-SDR");
    require(!engine.start(c,false,error)&&error.find("permission")!=std::string::npos,
        "Valid RTL-SDR configuration must reach explicit hardware consent without opening USB");
    c.sample_rate=2400000;
    require(!engine.start(c,false,error)&&error.find("sample rate")!=std::string::npos,"Unsupported RTL sample rate must fail before USB access");
    c.sample_rate=1000000;c.survey_span_hz=800000;
    require(!engine.start(c,false,error)&&error.find("discovery")!=std::string::npos,"1 MS/s cannot claim fixed 2 MS/s discovery support");
    c.discover_lora=false;
    require(!engine.start(c,false,error)&&error.find("permission")!=std::string::npos,"1 MS/s spectrum survey may reach hardware consent");
    c.sample_rate=2000000;c.survey_span_hz=1600000;c.discover_lora=true;
    require(!engine.start(c,false,error)&&error.find("1.5 MHz")!=std::string::npos,"Discovery must enforce its whole guarded survey range");
    c.discover_lora=false;
    require(!engine.start(c,false,error)&&error.find("permission")!=std::string::npos,"Spectrum may use the wider 80 percent survey span");
    c.survey_span_hz=1600001;
    require(!engine.start(c,false,error)&&error.find("80%")!=std::string::npos,"RTL span cannot exceed its sampled usable range");
    c.survey_span_hz=1500000;c.center_hz=24000000;c.tuning_offset_hz=-1;
    require(!engine.start(c,false,error)&&error.find("24 and 1766")!=std::string::npos,"Corrected RTL tune must stay within the application receiver bounds");
    c.center_hz=907500000;c.tuning_offset_hz=900;
    require(ovmesh::tuned_center_hz(c)==907500900,"RTL Offset has the same signed Hz semantics as HackRF");
    c.amplifier=true;
    require(!engine.start(c,false,error)&&error.find("amplifier")!=std::string::npos,"HackRF amplifier setting cannot silently apply to RTL-SDR");
    c.amplifier=false;c.device_serial=std::string("RTL\0other",9);
    require(!engine.start(c,false,error)&&error.find("serial")!=std::string::npos,"Embedded NUL cannot alter hardware selection");
    require(!engine.snapshot().running&&engine.snapshot().delivered_samples==0,"Validation tests must leave all USB input unopened");
}
}
int main() {
    try {sample_checks();lifecycle_checks();waveform_checks();config_checks();std::cout<<"RTL IQ, gain, cancellation, waveform and configuration checks passed\n";return 0;}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
