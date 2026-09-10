// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/phy.hpp"
#include "../third_party/sdrangel/lora_codec.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace ovmesh {
void fft_inplace(std::span<std::complex<float>> a,bool inverse) {
    const std::size_t n=a.size();
    if(n==0 || !std::has_single_bit(n)) throw std::invalid_argument("FFT length must be a power of two");
    for(std::size_t i=1,j=0;i<n;++i) {
        std::size_t bit=n>>1;
        for(;j&bit;bit>>=1) j^=bit;
        j^=bit;
        if(i<j) std::swap(a[i],a[j]);
    }
    for(std::size_t len=2;len<=n;) {
        const double angle=(inverse?2:-2)*std::numbers::pi/static_cast<double>(len);
        const std::complex<double> step=std::polar(1.0,angle);
        for(std::size_t base=0;base<n;base+=len) {
            std::complex<double> w{1,0};
            for(std::size_t j=0;j<len/2;++j) {
                const auto u=a[base+j];
                const auto v=a[base+j+len/2]*static_cast<std::complex<float>>(w);
                a[base+j]=u+v; a[base+j+len/2]=u-v;
                w*=step;
            }
        }
        if(len==n) break;
        len*=2;
    }
    if(inverse) for(auto& v:a) v/=static_cast<float>(n);
}

struct LoRaReceiver::Impl {
    using Complex=std::complex<float>;
    struct Peak { int bin=0; float ratio=0; float snr=0; };
    enum class State { Search, Sync, Down1, Down2, Quarter, Payload };
    PhyConfig config;
    PhyDiagnostics diagnostics;
    unsigned n;
    std::vector<Complex> chirp,fft,buffer;
    std::vector<std::uint16_t> symbols;
    std::uint64_t base=0,first=0;
    std::size_t cursor=0;
    State state=State::Search;
    unsigned stable=0,sync_count=0,expected=0,sync_wait=0;
    int old_bin=0,down_bin=0,cfo_int=0;
    double cfo_fraction=0;
    float snr=0;

    explicit Impl(PhyConfig c):config(c),n(c.spreading_factor>=7&&c.spreading_factor<=12?1U<<c.spreading_factor:0) {
        if(!n || c.coding_rate<5 || c.coding_rate>8 ||
            (c.bandwidth_hz!=125000&&c.bandwidth_hz!=250000&&c.bandwidth_hz!=500000))
            throw std::invalid_argument("Unsupported LoRa profile");
        chirp.resize(n); fft.resize(n); buffer.reserve(12*n); symbols.reserve(1024);
        for(unsigned i=0;i<n;++i) {
            const double phase=std::numbers::pi*(static_cast<double>(i)*i/n-i);
            chirp[i]=Complex(static_cast<float>(std::cos(phase)),static_cast<float>(std::sin(phase)));
        }
    }
    ~Impl() {
        std::fill(buffer.begin(),buffer.end(),Complex{});
        std::fill(fft.begin(),fft.end(),Complex{});
        std::fill(symbols.begin(),symbols.end(),std::uint16_t{});
    }
    static int wrap(int value,int size) { return (value%size+size)%size; }
    static int sync_distance(int bin,int target) {
        // Sync chirps carry four-bit nibbles at eight-bin spacing. SDRangel's
        // Meshtastic sink at 866ef1656af7e0923554581afc5c6dd97cbafbaf derives
        // each nibble as round(raw_bin/8)&0xf. Thus SF11 bins 2008 (-40) and
        // 88 both encode B. Compare within that 128-bin wire-format period;
        // the existing +/-2-bin tolerance remains unchanged.
        constexpr int period=16*8;
        return std::abs(wrap(bin-target+period/2,period)-period/2);
    }
    static void increment(std::uint64_t& value) noexcept {
        if(value<std::numeric_limits<std::uint64_t>::max())++value;
    }
    int signed_bin(int v) const { return v>=static_cast<int>(n/2)?v-static_cast<int>(n):v; }
    int distance(int a,int b) const { return std::abs(signed_bin(wrap(a-b,static_cast<int>(n)))); }
    Peak peak(std::size_t at,bool down=false,double correction=0) {
        const Complex step=std::polar(1.0f,static_cast<float>(-2*std::numbers::pi*correction/n));
        Complex rotation{1,0};
        for(unsigned i=0;i<n;++i) {
            fft[i]=buffer[at+i]*(down?chirp[i]:std::conj(chirp[i]))*rotation;
            rotation*=step;
            if((i&255)==255) rotation=std::polar(1.0f,static_cast<float>(-2*std::numbers::pi*correction*(i+1)/n));
        }
        fft_inplace(fft);
        double total=0,best=0; int index=0;
        for(unsigned i=0;i<n;++i) {
            const double power=std::norm(fft[i]); total+=power;
            if(power>best) { best=power; index=static_cast<int>(i); }
        }
        if(!std::isfinite(total)||total<1e-20) return {};
        // Exclude neighboring leakage bins from the noise estimate. This is a receiver
        // estimator, not calibrated RF power and not a substitute for a sensitivity test.
        const auto peak_index=static_cast<unsigned>(index);
        const double near=std::norm(fft[(peak_index+n-1)%n])+std::norm(fft[(peak_index+1)%n]);
        const double noise=std::max(1e-20,(total-best-near)/std::max(1U,n-3));
        const double signal=std::max(1e-20,best-noise);
        return {index,static_cast<float>(best/total),static_cast<float>(10*std::log10(signal/(noise*n)))};
    }
    void restart() {
        state=State::Search; stable=0; sync_count=0; sync_wait=0; expected=0;
        cfo_int=0; cfo_fraction=0;
        std::fill(symbols.begin(),symbols.end(),std::uint16_t{});
        symbols.clear();
    }
    void compact() {
        // Search keeps one symbol for repeat correlation; synchronization and payload
        // keep only the current unread window. Never retain a complete IQ packet.
        const std::size_t keep=state==State::Search?n:0;
        if(cursor>keep+4*n) {
            const std::size_t count=std::min(cursor-keep,buffer.size());
            const auto remaining=buffer.size()-count;
            std::move(buffer.begin()+static_cast<std::ptrdiff_t>(count),buffer.end(),buffer.begin());
            std::fill(buffer.begin()+static_cast<std::ptrdiff_t>(remaining),buffer.end(),Complex{});
            buffer.resize(remaining);
            cursor-=count; base+=count;
        }
    }
    void process(const std::function<void(PhyFrame&&)>& callback) {
        while(cursor+n<=buffer.size()) {
            if(state==State::Search) {
                const auto p=peak(cursor);
                const bool coherent=p.ratio>0.08f;
                if(coherent && stable && distance(p.bin,old_bin)<=1) ++stable;
                else stable=coherent?1:0;
                old_bin=p.bin;
                if(stable>=4) {
                    increment(diagnostics.preamble_candidates);
                    std::complex<double> correlation{0,0};
                    if(cursor>=n) for(unsigned i=0;i<n;++i)
                        correlation+=static_cast<std::complex<double>>(buffer[cursor+i]*std::conj(buffer[cursor-n+i]));
                    cfo_fraction=std::arg(correlation)/(2*std::numbers::pi);
                    snr=p.snr; first=base+cursor-3*n;
                    // The preamble FFT offset combines timing and integer CFO. Move
                    // to its next apparent boundary; the SFD separates CFO below.
                    cursor+=n+static_cast<unsigned>(wrap(-p.bin,static_cast<int>(n)));
                    state=State::Sync; sync_count=0; sync_wait=0;
                } else cursor+=n;
            } else if(state==State::Sync) {
                const auto p=peak(cursor,false,cfo_fraction);
                if(p.ratio<0.06f) {
                    increment(diagnostics.sync_rejections); increment(diagnostics.sync_low_ratio);
                    restart(); cursor+=n;
                }
                else if(++sync_wait>80) {
                    increment(diagnostics.sync_rejections); increment(diagnostics.sync_timeout);
                    restart(); cursor+=n;
                }
                else if(sync_count==0 && distance(p.bin,0)<=1) cursor+=n;
                else {
                    const unsigned target=sync_count==0?(config.sync_word>>4)*8:(config.sync_word&15)*8;
                    if(sync_distance(p.bin,static_cast<int>(target))>2) {
                        increment(diagnostics.sync_rejections);
                        increment(sync_count==0?diagnostics.sync_first_mismatch:diagnostics.sync_second_mismatch);
                        restart(); cursor+=n;
                    }
                    else { ++sync_count; cursor+=n; if(sync_count==2) { increment(diagnostics.sync_matches); state=State::Down1; } }
                }
            } else if(state==State::Down1 || state==State::Down2) {
                const auto p=peak(cursor,true,cfo_fraction);
                if(p.ratio<0.06f) { restart(); cursor+=n; }
                else {
                    down_bin=signed_bin(p.bin); cursor+=n;
                    if(state==State::Down1) state=State::Down2;
                    else { cfo_int=static_cast<int>(std::floor(down_bin/2.0)); state=State::Quarter; }
                }
            } else if(state==State::Quarter) {
                const auto advance=static_cast<std::ptrdiff_t>(n/4)+cfo_int;
                if(advance<0 || advance>static_cast<std::ptrdiff_t>(n/2)) { restart(); }
                else { cursor+=static_cast<std::size_t>(advance); state=State::Payload; symbols.clear(); }
            } else {
                const auto p=peak(cursor,false,cfo_fraction+cfo_int);
                symbols.push_back(static_cast<std::uint16_t>(p.bin)); cursor+=n;
                if(symbols.size()==8) {
                    const auto h=lora_detail::decode_header(symbols,config.spreading_factor,config.bandwidth_hz);
                    if(!h.valid) { increment(diagnostics.headers_failed); restart(); continue; }
                    increment(diagnostics.headers_valid);
                    expected=h.symbols;
                }
                if(expected && symbols.size()==expected) {
                    auto frame=decode_lora_symbols(symbols,config);
                    if(frame) {
                        increment(diagnostics.completed_frames);
                        frame->first_sample=first; frame->last_sample=base+cursor;
                        frame->snr_db=snr;
                        frame->frequency_error_hz=static_cast<float>((cfo_int+cfo_fraction)*config.bandwidth_hz/n);
                        restart(); callback(std::move(*frame));
                    } else restart();
                }
                if(symbols.size()>1024) restart();
            }
            compact();
        }
    }
};

LoRaReceiver::LoRaReceiver(PhyConfig config):impl_(std::make_unique<Impl>(config)) {}
LoRaReceiver::~LoRaReceiver()=default;
LoRaReceiver::LoRaReceiver(LoRaReceiver&&) noexcept=default;
LoRaReceiver& LoRaReceiver::operator=(LoRaReceiver&&) noexcept=default;
PhyDiagnostics LoRaReceiver::diagnostics() const noexcept { return impl_->diagnostics; }
void LoRaReceiver::reset() {
    std::fill(impl_->buffer.begin(),impl_->buffer.end(),std::complex<float>{});
    std::fill(impl_->fft.begin(),impl_->fft.end(),std::complex<float>{});
    impl_->buffer.clear(); impl_->cursor=0; impl_->base=0; impl_->restart();
}
void LoRaReceiver::feed(std::span<const std::complex<float>> samples,const std::function<void(PhyFrame&&)>& on_frame) {
    // Feed in bounded slices even if callers hand us a very large capture block.
    while(!samples.empty()) {
        const auto count=std::min<std::size_t>(impl_->n,samples.size());
        for(auto v:samples.first(count)) {
            if(!std::isfinite(v.real())||!std::isfinite(v.imag())) {
                reset(); impl_->buffer.push_back({});
            } else impl_->buffer.push_back(v);
        }
        samples=samples.subspan(count); impl_->process(on_frame);
    }
}
} // namespace ovmesh
