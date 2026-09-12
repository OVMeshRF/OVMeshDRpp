// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2020 Edouard Griffiths, F4EXB <f4exb06@gmail.com>
// Copyright (C) 2026 OVMeshDRpp contributors
// Adapted selected SDRangel LoRa codec logic; see third_party/sdrangel/PROVENANCE.md.
#include "ovmesh/phy.hpp"
#include "../third_party/sdrangel/lora_codec.hpp"
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace ovmesh {
namespace {
void validate(PhyConfig c) {
    if(c.spreading_factor<7 || c.spreading_factor>12 || c.coding_rate<5 || c.coding_rate>8 ||
        !supported_lora_bandwidth(c.bandwidth_hz))
        throw std::invalid_argument("Unsupported LoRa profile");
}
}

std::optional<PhyFrame> decode_lora_symbols(std::span<const std::uint16_t> bins, PhyConfig c) {
    validate(c);
    const auto h=lora_detail::decode_header(bins,c.spreading_factor,c.bandwidth_hz);
    if(!h.valid || bins.size()<h.symbols) return std::nullopt;
    const unsigned n=1U<<c.spreading_factor;
    std::vector<std::uint8_t> nibbles;
    nibbles.reserve(5+2*(h.length+(h.crc?2:0))+12);
    bool corrected=h.corrected;
    for(unsigned offset=0;offset<h.symbols;) {
        const bool header=offset==0;
        const unsigned parity=header?4:h.parity;
        const unsigned div=(header||lora_detail::ldro(c.spreading_factor,c.bandwidth_hz))?4:1;
        const unsigned width=c.spreading_factor-(div==4?2:0);
        std::array<std::uint16_t,8> symbols{};
        for(unsigned i=0;i<4+parity;++i) {
            if(bins[offset+i]>=n) return std::nullopt;
            const unsigned s=((bins[offset+i]+n-1)%n)/div;
            symbols[i]=static_cast<std::uint16_t>(s^(s>>1));
        }
        const auto cw=lora_detail::deinterleave(std::span(symbols).first(4+parity),width,parity);
        for(unsigned row=0;row<width;++row) {
            auto decoded=lora_detail::decode_nibble(cw[row],parity);
            // Preserve a CRC-failed frame for structural RF reporting, but do not claim correction.
            // An uncorrectable codeword's systematic nibble is sufficient for that report.
            if(decoded.invalid) decoded.value=lora_detail::reverse4(cw[row]>>parity);
            else corrected|=decoded.corrected;
            nibbles.push_back(decoded.value);
        }
        offset+=4+parity;
    }
    const unsigned count=h.length+(h.crc?2:0);
    if(nibbles.size()<5+2*count) return std::nullopt;
    std::vector<std::uint8_t> bytes(count);
    for(unsigned i=0;i<count;++i) bytes[i]=static_cast<std::uint8_t>(nibbles[5+2*i]|(nibbles[6+2*i]<<4));
    lora_detail::whiten(std::span(bytes).first(h.length));
    PhyFrame frame;
    frame.header_valid=true;
    frame.payload_crc_present=h.crc;
    frame.fec_corrected=corrected;
    frame.coding_rate=static_cast<std::uint8_t>(4+h.parity);
    if(h.crc && h.length>=2) {
        const auto received=static_cast<std::uint16_t>(bytes[h.length]|(bytes[h.length+1]<<8));
        frame.payload_crc_valid=received==lora_detail::payload_crc(std::span(bytes).first(h.length));
    }
    bytes.resize(h.length);
    frame.bytes=std::move(bytes);
    return frame;
}

std::vector<std::uint16_t> encode_lora_symbols(std::span<const std::uint8_t> payload, PhyConfig c) {
    validate(c);
    if(payload.size()<2 || payload.size()>255) throw std::invalid_argument("LoRa fixture payload length must be 2..255");
    const auto flags=static_cast<std::uint8_t>(((c.coding_rate-4)<<1)|1);
    const auto check=lora_detail::header_checksum(static_cast<std::uint8_t>(payload.size()),flags);
    std::vector<std::uint8_t> nibbles={static_cast<std::uint8_t>(payload.size()>>4),
        static_cast<std::uint8_t>(payload.size()&15),static_cast<std::uint8_t>(flags),
        static_cast<std::uint8_t>(check>>4),static_cast<std::uint8_t>(check&15)};
    std::vector<std::uint8_t> bytes(payload.begin(),payload.end());
    const auto crc=lora_detail::payload_crc(payload);
    lora_detail::whiten(bytes);
    bytes.push_back(static_cast<std::uint8_t>(crc)); bytes.push_back(static_cast<std::uint8_t>(crc>>8));
    for(auto b:bytes) { nibbles.push_back(b&15); nibbles.push_back(b>>4); }
    std::vector<std::uint16_t> result;
    for(std::size_t offset=0;offset<nibbles.size();) {
        const bool header=offset==0;
        const unsigned parity=header?4:c.coding_rate-4;
        const unsigned div=(header||lora_detail::ldro(c.spreading_factor,c.bandwidth_hz))?4:1;
        const unsigned width=c.spreading_factor-(div==4?2:0);
        const auto symbols=lora_detail::interleave(std::span(nibbles).subspan(offset,
            std::min<std::size_t>(width,nibbles.size()-offset)),width,parity);
        for(auto gray:symbols) {
            std::uint16_t binary=gray;
            for(unsigned shift=1;shift<16;shift<<=1) binary^=binary>>shift;
            result.push_back(static_cast<std::uint16_t>((binary*div+1)%(1U<<c.spreading_factor)));
        }
        offset+=width;
    }
    return result;
}

std::vector<std::complex<float>> modulate_lora(std::span<const std::uint8_t> payload, PhyConfig c,
                                             std::uint32_t sample_rate_hz) {
    const auto symbols=encode_lora_symbols(payload,c);
    const unsigned n=1U<<c.spreading_factor;
    const bool legacy_chip_rate=sample_rate_hz==0;
    const uint32_t rate=legacy_chip_rate?c.bandwidth_hz:sample_rate_hz;
    if(rate<c.bandwidth_hz || rate>20000000 || rate%c.bandwidth_hz)
        throw std::invalid_argument("LoRa fixture rate must be an integer bandwidth multiple, at most 20 MS/s");
    const unsigned oversampling=rate/c.bandwidth_hz;
    constexpr uint64_t maximum_samples=16U*1024U*1024U;
    const uint64_t full_symbol_samples=uint64_t(n)*oversampling;
    const uint64_t output_samples=(symbols.size()+12)*full_symbol_samples+full_symbol_samples/4;
    if(output_samples>maximum_samples)
        throw std::invalid_argument("LoRa fixture exceeds the 128 MiB in-memory sample bound");
    std::vector<std::complex<float>> result;
    result.reserve(static_cast<std::size_t>(output_samples));
    const double pi=std::numbers::pi;
    double phase_carry=0;
    auto chirp=[&](unsigned symbol,bool down,unsigned length) {
        if(legacy_chip_rate) {
            // Retain existing chip-rate decoder fixtures byte-for-byte. Their
            // per-symbol cyclic-shift phase convention must not be used as a
            // zero-order-held wideband waveform.
            for(unsigned i=0;i<length;++i) {
                const unsigned shifted=(i+symbol)%n;
                double phase=pi*(static_cast<double>(shifted)*shifted/n-shifted);
                if(down)phase=-phase;
                result.emplace_back(static_cast<float>(std::cos(phase)),static_cast<float>(std::sin(phase)));
            }
            return;
        }
        // Integrate the wrapped instantaneous chirp frequency, in cycles.
        // This is the continuous-phase form of the published LoRa waveform
        // (Chiani/Elzanaty 2019, Eq.5), evaluated at every requested sample.
        // A wrap changes this phase by an integer number of cycles. Unlike a
        // cyclic-shift-only expression, symbols share a continuous endpoint.
        const auto cycles=[&](double chip) {
            const double wrap=chip>=n-symbol?1.:0.;
            const double value=chip*(static_cast<double>(symbol)/n-.5+chip/(2*n)-wrap);
            return down?-value:value;
        };
        for(unsigned i=0;i<length*oversampling;++i) {
            const double phase=2*pi*std::remainder(phase_carry+cycles(static_cast<double>(i)/oversampling),1.);
            result.emplace_back(static_cast<float>(std::cos(phase)),static_cast<float>(std::sin(phase)));
        }
        // Carry the quarter-symbol delimiter endpoint too, rather than insert
        // a gap or advance the next payload symbol to a full-symbol boundary.
        phase_carry=std::remainder(phase_carry+cycles(length),1.);
    };
    for(unsigned i=0;i<8;++i) chirp(0,false,n);
    chirp((c.sync_word>>4)*8,false,n); chirp((c.sync_word&15)*8,false,n);
    chirp(0,true,n); chirp(0,true,n); chirp(0,true,n/4);
    for(auto symbol:symbols) chirp(symbol,false,n);
    return result;
}
} // namespace ovmesh
