#include "ovmesh/phy.hpp"
#include "lora_discovery_fixtures.hpp"
#include "../third_party/sdrangel/lora_codec.hpp"
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <random>
#include <stdexcept>

namespace {
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void primitive_vectors() {
    // CRC-16/XMODEM check value, independently specified for ASCII "123456789".
    const std::array<std::uint8_t,9> digits{'1','2','3','4','5','6','7','8','9'};
    require(ovmesh::lora_detail::crc_ccitt(digits)==0x31c3,"CCITT reference check value");
    // Frozen start of SDRangel/gr-lora_sdr whitening table, independent of LFSR implementation.
    std::array<std::uint8_t,16> whitening{};
    ovmesh::lora_detail::whiten(whitening);
    require(whitening==std::array<std::uint8_t,16>{0xff,0xfe,0xfc,0xf8,0xf0,0xe1,0xc2,0x85,
        0x0b,0x17,0x2f,0x5e,0xbc,0x78,0xf1,0xe3},"Whitening reference sequence");
    require(ovmesh::lora_detail::decode_nibble(0x17,4).value==8,"Independent 4/8 codeword 0x17");
    require(ovmesh::lora_detail::decode_nibble(0x2d,4).value==4,"Independent 4/8 codeword 0x2d");
    require(ovmesh::lora_detail::decode_nibble(0x16,4).value==8,"Single bit correction");
    require(ovmesh::lora_detail::decode_nibble(0x14,4).invalid,"Double bit error rejection");
    // Header length 0x10, flags 0x03 gives parity bits 11101 by the five parity equations.
    require(ovmesh::lora_detail::header_checksum(16,3)==0x1d,"Header checksum fixed vector");
    // SF7 header length 16, CR4/5, CRC enabled. Independent frozen FFT-bin fixture
    // formed from source codewords 8b 00 c5 8b b1; it does not call our encoder.
    constexpr std::array<std::uint16_t,8> header_bins{89,13,29,13,113,29,97,41};
    const auto header=ovmesh::lora_detail::decode_header(header_bins,7,125000);
    require(header.valid && header.length==16 && header.parity==1 && header.crc && header.symbols==38,
        "Fixed explicit header symbol vector");
    require(!ovmesh::lora_detail::decode_header({},11,250000).valid,"Short header rejected");
}
void fft_vectors() {
    std::array<std::complex<float>,8> impulse{}; impulse[0]={1,0};
    ovmesh::fft_inplace(impulse);
    for(auto x:impulse) require(std::abs(x-std::complex<float>{1,0})<1e-6f,"FFT impulse");
    std::array<std::complex<float>,8> tone{};
    for(unsigned i=0;i<8;++i) tone[i]=std::polar(1.0f,static_cast<float>(2*std::numbers::pi*3*i/8));
    const auto original=tone; ovmesh::fft_inplace(tone);
    require(std::abs(tone[3]-std::complex<float>{8,0})<1e-5f,"FFT complex tone");
    for(unsigned i=0;i<8;++i) if(i!=3) require(std::abs(tone[i])<1e-5f,"FFT leakage");
    ovmesh::fft_inplace(tone,true);
    for(unsigned i=0;i<8;++i) require(std::abs(tone[i]-original[i])<1e-6f,"FFT inverse");
    bool threw=false;
    try { ovmesh::fft_inplace(std::span(tone).first(3)); } catch(const std::invalid_argument&) { threw=true; }
    require(threw,"Non-power-of-two FFT rejected");
}
std::vector<std::uint8_t> payload(unsigned length=43) {
    std::vector<std::uint8_t> bytes(length);
    for(unsigned i=0;i<length;++i) bytes[i]=static_cast<std::uint8_t>(i*37+9);
    return bytes;
}
void symbol_roundtrips() {
    for(unsigned sf=7;sf<=12;++sf) for(unsigned cr=5;cr<=8;++cr) for(unsigned bw:{15625U,62500U,125000U,250000U,500000U}) {
        ovmesh::PhyConfig c{bw,static_cast<std::uint8_t>(sf),static_cast<std::uint8_t>(cr),0x2b};
        for(unsigned length:{2U,43U,255U}) {
            const auto data=payload(length);
            const auto bins=ovmesh::encode_lora_symbols(data,c);
            const auto decoded=ovmesh::decode_lora_symbols(bins,c);
            require(decoded && decoded->header_valid && decoded->payload_crc_present && decoded->payload_crc_valid &&
                decoded->bytes==data && decoded->coding_rate==cr,"Symbol roundtrip");
            require(!ovmesh::decode_lora_symbols(std::span(bins).first(7),c),"Partial header rejected");
        }
    }
}
void iq_roundtrip(ovmesh::PhyConfig c,unsigned leading,double cfo_bins,float sigma) {
    const auto data=payload();
    auto signal=ovmesh::modulate_lora(data,c);
    signal.insert(signal.begin(),leading,{});
    signal.resize(signal.size()+(1U<<c.spreading_factor)*2);
    std::mt19937 random(17); std::normal_distribution<float> noise(0,sigma);
    for(std::size_t i=0;i<signal.size();++i) {
        signal[i]*=std::polar(1.0f,static_cast<float>(2*std::numbers::pi*cfo_bins*i/(1U<<c.spreading_factor)));
        signal[i]+=std::complex<float>{noise(random),noise(random)};
    }
    ovmesh::LoRaReceiver rx(c); unsigned count=0;
    auto receive=[&](ovmesh::PhyFrame&& f) {
        require(f.bytes==data && f.header_valid && f.payload_crc_valid,"IQ frame content or CRC");
        require(f.last_sample>f.first_sample,"Sample interval");
        require(std::abs(f.frequency_error_hz-cfo_bins*c.bandwidth_hz/(1U<<c.spreading_factor))<
            0.15*c.bandwidth_hz/(1U<<c.spreading_factor),"CFO estimate");
        ++count;
    };
    std::size_t offset=0;
    while(offset<signal.size()) {
        const auto size=std::min<std::size_t>(1+(offset*17)%709,signal.size()-offset);
        rx.feed(std::span(signal).subspan(offset,size),receive); offset+=size;
    }
    if(count!=1) std::cerr<<"Missing frame sf="<<unsigned(c.spreading_factor)<<" lead="<<leading<<" CFO="<<cfo_bins<<" noise="<<sigma<<"\n";
    require(count==1,"One incremental IQ frame expected");
}
void stream_rejection() {
    ovmesh::PhyConfig c;
    auto signal=ovmesh::modulate_lora(payload(),c);
    ovmesh::LoRaReceiver rx(c); unsigned count=0;
    auto receive=[&](ovmesh::PhyFrame&&) { ++count; };
    rx.feed(std::span(signal).first(signal.size()/2),receive); rx.reset();
    rx.feed(std::span(signal).subspan(signal.size()/2),receive);
    require(count==0,"Gap must discard partial frame");
    rx.reset(); c.sync_word=0x34;
    signal=ovmesh::modulate_lora(payload(),c); rx.feed(signal,receive);
    require(count==0,"Different sync word rejected");
    rx.reset(); std::mt19937 random(5); std::normal_distribution<float> noise(0,1);
    std::array<std::complex<float>,4096> block{};
    for(unsigned i=0;i<50;++i) { for(auto& s:block) s={noise(random),noise(random)}; rx.feed(block,receive); }
    require(count==0,"Noise must not produce frames");
    // A reset receiver can recover on a subsequent complete packet.
    rx.reset(); c.sync_word=0x2b;
    signal=ovmesh::modulate_lora(payload(),c);
    signal.resize(signal.size()+(1U<<c.spreading_factor));
    rx.feed(signal,receive); rx.feed(signal,receive);
    require(count==2,"Consecutive complete packets");
}
void aggregate_diagnostics() {
    const ovmesh::PhyConfig c{250000,7,5,0x2b};
    ovmesh::LoRaReceiver rx(c);
    require(rx.diagnostics().preamble_candidates==0 && rx.diagnostics().completed_frames==0,"New receiver counters start at zero");
    unsigned delivered=0;
    auto receive=[&](ovmesh::PhyFrame&&) { ++delivered; };
    const auto good=ovmesh::modulate_lora(payload(),c);
    rx.feed(good,receive);
    const auto accepted=rx.diagnostics();
    require(accepted.preamble_candidates==1 && accepted.sync_matches==1 && accepted.sync_rejections==0 &&
        accepted.headers_valid==1 && accepted.headers_failed==0 && accepted.completed_frames==1 && delivered==1,
        "Successful frame acquisition counters");
    rx.reset();
    require(rx.diagnostics().preamble_candidates==1 && rx.diagnostics().completed_frames==1,"Gap reset preserves aggregate counters");
    auto other=c;other.sync_word=0x34;
    rx.feed(ovmesh::modulate_lora(payload(),other),receive);
    require(rx.diagnostics().preamble_candidates>accepted.preamble_candidates && rx.diagnostics().sync_rejections>0 &&
        rx.diagnostics().sync_matches==accepted.sync_matches && delivered==1,"Wrong sync word increments rejection without a frame");
    rx.reset();
    auto bad_header=good;
    constexpr unsigned n=1U<<7;
    // Keep the preamble/sync/SFD intact; remove eight explicit-header chirps.
    std::fill(bad_header.begin()+12*n+n/4,bad_header.begin()+20*n+n/4,std::complex<float>{});
    rx.feed(bad_header,receive);
    const auto rejected=rx.diagnostics();
    require(rejected.sync_matches==2 && rejected.headers_failed>=1 && rejected.headers_valid==1 &&
        rejected.completed_frames==1 && delivered==1,"Invalid header remains aggregate metadata only");
}
void sync_rejection_reasons() {
    const ovmesh::PhyConfig c{250000,7,5,0x2b};
    constexpr std::size_t n=1U<<7;
    const auto good=ovmesh::modulate_lora(payload(),c);
    auto inspect=[&](const std::vector<std::complex<float>>& signal,unsigned expected_frames) {
        ovmesh::LoRaReceiver rx(c);unsigned frames=0;
        rx.feed(signal,[&](ovmesh::PhyFrame&& f){require(f.payload_crc_valid,"Diagnostic fixture CRC");++frames;});
        require(frames==expected_frames,"Diagnostic fixture frame count");
        const auto d=rx.diagnostics();
        require(d.sync_rejections==d.sync_low_ratio+d.sync_timeout+d.sync_first_mismatch+d.sync_second_mismatch,
            "Each sync rejection has exactly one aggregate reason");
        rx.reset();
        const auto reset=rx.diagnostics();
        require(reset.sync_low_ratio==d.sync_low_ratio && reset.sync_timeout==d.sync_timeout &&
            reset.sync_first_mismatch==d.sync_first_mismatch && reset.sync_second_mismatch==d.sync_second_mismatch,
            "Reason counters survive stream reset");
        return d;
    };
    auto first=c;first.sync_word=0x3b;
    const auto first_reject=inspect(ovmesh::modulate_lora(payload(),first),0);
    require(first_reject.sync_first_mismatch==1 && first_reject.sync_second_mismatch==0 &&
        first_reject.sync_low_ratio==0 && first_reject.sync_timeout==0,"Wrong first sync nibble isolated");
    auto second=c;second.sync_word=0x2a;
    const auto second_reject=inspect(ovmesh::modulate_lora(payload(),second),0);
    require(second_reject.sync_first_mismatch==0 && second_reject.sync_second_mismatch==1 &&
        second_reject.sync_low_ratio==0 && second_reject.sync_timeout==0,"Wrong second sync nibble isolated");
    std::vector<std::complex<float>> vanished(good.begin(),good.begin()+8*n);
    vanished.resize(12*n);
    const auto lost=inspect(vanished,0);
    require(lost.sync_low_ratio==1 && lost.sync_timeout==0 && lost.sync_first_mismatch==0 &&
        lost.sync_second_mismatch==0,"Lost coherent signal after preamble isolated");
    std::vector<std::complex<float>> long_preamble(96*n);
    for(std::size_t i=0;i<long_preamble.size();++i)long_preamble[i]=good[i%n];
    long_preamble.insert(long_preamble.end(),good.begin(),good.end());
    const auto timeout=inspect(long_preamble,1);
    require(timeout.sync_timeout==1 && timeout.sync_low_ratio==0 && timeout.sync_first_mismatch==0 &&
        timeout.sync_second_mismatch==0 && timeout.completed_frames==1,"Bounded sync wait expires and recovers on the later frame");
}
ovmesh::PhyDiagnostics sync_bins_fixture(ovmesh::PhyConfig c,int first_bin,int second_bin,bool expect_frame) {
    const auto data=payload();
    auto signal=ovmesh::modulate_lora(data,c);
    const unsigned n=1U<<c.spreading_factor;
    auto replace_sync=[&](unsigned slot,int signed_bin) {
        const unsigned bin=static_cast<unsigned>((signed_bin%static_cast<int>(n)+static_cast<int>(n))%static_cast<int>(n));
        const double initial=std::numbers::pi*(static_cast<double>(bin)*bin/n-bin);
        for(unsigned i=0;i<n;++i) {
            const unsigned shifted=(i+bin)%n;
            const double phase=std::numbers::pi*(static_cast<double>(shifted)*shifted/n-shifted)-initial;
            signal[static_cast<std::size_t>(8+slot)*n+i]={static_cast<float>(std::cos(phase)),static_cast<float>(std::sin(phase))};
        }
    };
    // The sync chirps are generated directly from requested FFT offsets, without
    // using the encoder's positive-nibble mapping. Subtract initial phase to
    // preserve continuous phase at each sync boundary.
    replace_sync(0,first_bin);replace_sync(1,second_bin);
    ovmesh::LoRaReceiver rx(c);unsigned frames=0;
    for(std::size_t at=0;at<signal.size();) {
        const auto size=std::min<std::size_t>(113,signal.size()-at);
        rx.feed(std::span(signal).subspan(at,size),[&](ovmesh::PhyFrame&& frame) {
            require(frame.bytes==data && frame.header_valid && frame.payload_crc_valid,"Sync-word equivalence fixture content and CRC");
            ++frames;
        });
        at+=size;
    }
    require(frames==(expect_frame?1U:0U),"Sync-word equivalence fixture frame count");
    return rx.diagnostics();
}
void sync_nibble_equivalence() {
    // SDRangel derives each nibble as round(raw_bin/8)&0xf. At SF11, low
    // nibble B may therefore appear at raw bin 2008 (-40), as well as 88.
    for(unsigned sf=7;sf<=12;++sf)for(const unsigned bw:{250000U,500000U}) {
        ovmesh::PhyConfig c{bw,static_cast<std::uint8_t>(sf),static_cast<std::uint8_t>(bw==250000?5:8),0x2b};
        require(sync_bins_fixture(c,16,-40,true).sync_matches==1,"Signed low nibble B accepted");
        require(sync_bins_fixture(c,16,88,true).sync_matches==1,"Positive low nibble B remains accepted");
        c.sync_word=0xb2;
        require(sync_bins_fixture(c,-40,16,true).sync_matches==1,"Signed high nibble B accepted");
        c.sync_word=0x2b;
        require(sync_bins_fixture(c,16,-48,false).sync_second_mismatch==1,"Neighboring nibble A remains rejected");
        require(sync_bins_fixture(c,16,-32,false).sync_second_mismatch==1,"Neighboring nibble C remains rejected");
    }
    const ovmesh::PhyConfig c;
    require(sync_bins_fixture(c,16,-42,true).sync_matches==1,"Original negative two-bin tolerance retained");
    require(sync_bins_fixture(c,16,-38,true).sync_matches==1,"Original positive two-bin tolerance retained");
    require(sync_bins_fixture(c,16,-43,false).sync_second_mismatch==1,"Three-bin negative residual remains rejected");
    require(sync_bins_fixture(c,16,-37,false).sync_second_mismatch==1,"Three-bin positive residual remains rejected");
    require(sync_bins_fixture(c,8,-40,false).sync_first_mismatch==1,"Wrong first nibble remains rejected");
    require(sync_bins_fixture(c,24,-32,false).sync_first_mismatch==1,"Different word 0x3C with the same pair spacing remains rejected");
    require(sync_bins_fixture(c,0,-40,false).sync_matches==0,"Zero-bin chirp remains preamble, not configured sync");
    auto zero_nibble=c;zero_nibble.sync_word=0x20;
    for(int second:{127,0,1})
        require(sync_bins_fixture(zero_nibble,16,second,true).sync_matches==1,"Zero low nibble handles the modulo boundary");
    require(sync_bins_fixture(zero_nibble,16,125,false).sync_second_mismatch==1,"Modulo-boundary negative residual outside tolerance rejected");
    require(sync_bins_fixture(zero_nibble,16,3,false).sync_second_mismatch==1,"Modulo-boundary positive residual outside tolerance rejected");
}
// Source-rate waveform checks use an independent equation for the preamble,
// SFD and given symbol indices; this does not claim independent FEC validation.
namespace analytic=lora_discovery_fixtures;
template<typename Work> void reject_modulation(Work work) {
    bool caught=false;try{work();}catch(const std::invalid_argument&){caught=true;}
    require(caught,"Invalid rate/output bound must reject before large sample allocation");
}
constexpr std::array<uint8_t,8> modulation_payload{0x17,0x92,0x00,0x51,0x03,0xae,0x44,0xf1};
void source_rate_modulation_case(ovmesh::PhyConfig config,uint32_t rate) {
    const auto legacy=ovmesh::modulate_lora(modulation_payload,config);
    const auto actual=ovmesh::modulate_lora(modulation_payload,config,rate);
    const auto symbols=ovmesh::encode_lora_symbols(modulation_payload,config);
    const unsigned samples_per_chip=rate/config.bandwidth_hz;
    require(actual.size()==legacy.size()*samples_per_chip,"Rate change preserves exact symbol and quarter-SFD durations");
    std::vector<unsigned> tail(symbols.begin(),symbols.end());
    analytic::PreambleSpec spec;spec.sample_rate_hz=rate;spec.bandwidth_hz=config.bandwidth_hz;
    spec.spreading_factor=config.spreading_factor;spec.preamble_symbols=8;spec.sync_word=config.sync_word;
    spec.leading_samples=0;spec.trailing_samples=0;spec.fractional_start_samples=0;
    spec.amplitude=1;spec.initial_phase_radians=0;spec.noise_rms=0;
    const auto expected=analytic::make_preamble_with_chirp_tail(spec,tail);
    require(actual.size()==expected.samples.size(),"Preamble, sync, SFD and encoded-symbol boundaries match independent equation");
    double maximum_error=0;size_t varied=0;
    for(size_t i=0;i<actual.size();++i) {
        maximum_error=std::max(maximum_error,static_cast<double>(std::abs(actual[i]-expected.samples[i])));
        require(std::abs(std::abs(actual[i])-1.f)<1e-6f,"Synthetic modulator keeps unit envelope");
        if(i%samples_per_chip&&std::abs(actual[i]-actual[i-i%samples_per_chip])>.01f)++varied;
    }
    require(maximum_error<5e-5,"Source-rate chirps disagree with independent continuous-phase analytic waveform");
    require(varied>actual.size()/2,"Source-rate waveform must not repeat held chip samples");
    std::vector<std::complex<float>> chips;chips.reserve(legacy.size()+2*(1u<<config.spreading_factor));
    for(size_t i=0;i<actual.size();i+=samples_per_chip)chips.push_back(actual[i]);
    const auto explicit_chip_rate=ovmesh::modulate_lora(modulation_payload,config,config.bandwidth_hz);
    require(std::equal(chips.begin(),chips.end(),explicit_chip_rate.begin(),[](auto a,auto b){return std::abs(a-b)<5e-5f;}),"Explicit source rates preserve chip-aligned phase convention");
    chips.resize(chips.size()+2*(1u<<config.spreading_factor));
    ovmesh::LoRaReceiver decoder(config);unsigned decoded=0;
    decoder.feed(chips,[&](ovmesh::PhyFrame&& frame) {
        require(frame.header_valid&&frame.payload_crc_valid&&std::equal(frame.bytes.begin(),frame.bytes.end(),modulation_payload.begin(),modulation_payload.end()),"Continuous-phase waveform retains valid decoded payload");
        ++decoded;
    });
    require(decoded==1,"Continuous-phase waveform decodes exactly once at chip rate");
    std::cout<<"Analytic modulation BW="<<config.bandwidth_hz<<" SF="<<unsigned(config.spreading_factor)
             <<" rate="<<rate<<" samples="<<actual.size()<<" max_complex_error="<<maximum_error<<'\n';
}
void source_rate_modulation() {
    source_rate_modulation_case({250000,11,5,0x2b},8000000);
    source_rate_modulation_case({500000,11,5,0x2b},8000000);
    source_rate_modulation_case({125000,7,8,0x34},2000000);
    source_rate_modulation_case({500000,7,5,0x12},20000000);
    ovmesh::PhyConfig c;
    for(uint32_t rate:{124999u,249999u,250001u,8000001u,21000000u,UINT32_MAX})
        reject_modulation([&]{(void)ovmesh::modulate_lora(modulation_payload,c,rate);});
    std::array<uint8_t,255> largest{};c={125000,12,8,0x2b};
    reject_modulation([&]{(void)ovmesh::modulate_lora(largest,c,20000000);});
}
}
int main() {
    try {
        primitive_vectors(); fft_vectors(); symbol_roundtrips(); source_rate_modulation();
        for(unsigned sf=7;sf<=12;++sf) {
            const ovmesh::PhyConfig c{250000,static_cast<std::uint8_t>(sf),5,0x2b};
            iq_roundtrip(c,0,0,0);
            iq_roundtrip(c,(1U<<sf)/3+7,2.23,0.2f);
            iq_roundtrip(c,(1U<<sf)*3/4,-3.31,0.3f);
        }
        for(double cfo:{-30.49,-2.51,-0.51,-0.49,0.49,0.51,2.51,30.49})
            iq_roundtrip(ovmesh::PhyConfig{},311,cfo,0.2f);
        stream_rejection(); aggregate_diagnostics(); sync_rejection_reasons(); sync_nibble_equivalence();
        std::cout<<"Native LoRa PHY tests passed\n";
        return EXIT_SUCCESS;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return EXIT_FAILURE; }
}
