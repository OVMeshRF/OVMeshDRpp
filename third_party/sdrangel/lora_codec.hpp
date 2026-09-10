// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2020 Edouard Griffiths, F4EXB <f4exb06@gmail.com>
// Copyright (C) 2026 OVMeshDRpp contributors
// Adapted from SDRangel MeshtasticDemodDecoderLoRa at
// 866ef1656af7e0923554581afc5c6dd97cbafbaf. See PROVENANCE.md.
// This program is distributed WITHOUT ANY WARRANTY; see LICENSE.
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <vector>

namespace ovmesh::lora_detail {
inline constexpr std::array<std::uint8_t, 16> codeword84 = {
    0, 23, 45, 58, 78, 89, 99, 116, 139, 156, 166, 177, 197, 210, 232, 255};
inline constexpr std::array<std::uint8_t, 16> codeword45 = {
    0, 24, 40, 48, 72, 80, 96, 120, 136, 144, 160, 184, 192, 216, 232, 240};
inline std::uint8_t reverse4(std::uint8_t v) {
    return static_cast<std::uint8_t>(((v & 1) << 3) | ((v & 2) << 1) |
                                     ((v & 4) >> 1) | ((v & 8) >> 3));
}
inline std::uint8_t encode_nibble(std::uint8_t nibble, unsigned parity) {
    return static_cast<std::uint8_t>((parity == 1 ? codeword45 : codeword84)
        [reverse4(nibble & 15)] >> (4 - parity));
}
struct Nibble { std::uint8_t value = 0; bool corrected = false; bool invalid = false; };
inline Nibble decode_nibble(std::uint8_t cw, unsigned parity) {
    if (parity < 1 || parity > 4) return {0, false, true};
    unsigned best = 9, ties = 0;
    std::uint8_t result = 0;
    for (unsigned n = 0; n < 16; ++n) {
        const auto candidate = encode_nibble(static_cast<std::uint8_t>(n), parity);
        const unsigned distance = static_cast<unsigned>(std::popcount(static_cast<unsigned>(cw ^ candidate)));
        if (distance < best) { best = distance; ties = 1; result = static_cast<std::uint8_t>(n); }
        else if (distance == best) ++ties;
    }
    // Rates 4/5 and 4/6 detect but cannot uniquely correct every single-bit error.
    // Even rates 4/7 and 4/8 must not silently accept an ambiguous nearest word.
    return {result, best != 0, ties != 1 || best > (parity >= 3 ? 1U : 0U)};
}
inline std::uint8_t header_checksum(std::uint8_t length, std::uint8_t flags) {
    const unsigned a0=(length>>4)&1, a1=(length>>5)&1, a2=(length>>6)&1, a3=(length>>7)&1;
    const unsigned b0=length&1, b1=(length>>1)&1, b2=(length>>2)&1, b3=(length>>3)&1;
    const unsigned c0=flags&1, c1=(flags>>1)&1, c2=(flags>>2)&1, c3=(flags>>3)&1;
    return static_cast<std::uint8_t>(((a0^a1^a2^a3)<<4) | ((a3^b1^b2^b3^c0)<<3) |
        ((a2^b0^b3^c1^c3)<<2) | ((a1^b0^b2^c0^c1^c2)<<1) | (a0^b1^c0^c1^c2^c3));
}
inline std::uint16_t crc_ccitt(std::span<const std::uint8_t> bytes) {
    std::uint16_t crc = 0;
    for (auto b : bytes) {
        crc ^= static_cast<std::uint16_t>(b << 8);
        for (unsigned i=0; i<8; ++i)
            crc = static_cast<std::uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}
inline std::uint16_t payload_crc(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 2) return 0;
    return static_cast<std::uint16_t>(crc_ccitt(bytes.first(bytes.size()-2)) ^ bytes.back() ^
        (static_cast<std::uint16_t>(bytes[bytes.size()-2]) << 8));
}
inline void whiten(std::span<std::uint8_t> bytes) {
    // The 255-byte whitening table in SDRangel is the output of this 8-bit LFSR.
    // Feedback polynomial x^8+x^6+x^5+x^4+1, initial register 0xff.
    std::uint8_t state=0xff;
    for (auto& b : bytes) {
        b ^= state;
        const unsigned feedback=((state>>7)^(state>>5)^(state>>4)^(state>>3))&1;
        state=static_cast<std::uint8_t>((static_cast<unsigned>(state)<<1)|feedback);
    }
}
inline std::vector<std::uint8_t> deinterleave(std::span<const std::uint16_t> symbols,
                                              unsigned width, unsigned parity) {
    if (width < 5 || width > 12 || parity < 1 || parity > 4 || symbols.size() != 4+parity)
        return {};
    std::vector<std::uint8_t> codewords(width, 0);
    const unsigned length=4+parity;
    for (unsigned i=0; i<length; ++i)
        for (unsigned j=0; j<width; ++j) {
            const unsigned row=(i+2*width-j-1)%width;
            codewords[row] |= static_cast<std::uint8_t>(((symbols[i]>>(width-1-j))&1) << (length-1-i));
        }
    return codewords;
}
inline std::vector<std::uint16_t> interleave(std::span<const std::uint8_t> nibbles,
                                            unsigned width, unsigned parity) {
    const unsigned length=4+parity;
    std::vector<std::uint16_t> symbols(length, 0);
    for (unsigned i=0; i<length; ++i)
        for (unsigned j=0; j<width; ++j) {
            const unsigned row=(i+2*width-j-1)%width;
            const auto cw=encode_nibble(row<nibbles.size() ? nibbles[row] : 0, parity);
            symbols[i] |= static_cast<std::uint16_t>(((cw>>(length-1-i))&1) << (width-1-j));
        }
    return symbols;
}
struct Header {
    unsigned length=0, parity=0, symbols=0;
    bool crc=false, valid=false, corrected=false;
};
inline bool ldro(unsigned sf, unsigned bandwidth) {
    return (std::uint64_t{1} << sf)*1000 > std::uint64_t{16}*bandwidth;
}
inline Header decode_header(std::span<const std::uint16_t> bins, unsigned sf, unsigned bandwidth) {
    Header h;
    if (sf<7 || sf>12 || bins.size()<8 || bandwidth==0) return h;
    const unsigned n=1U<<sf;
    std::array<std::uint16_t,8> symbols{};
    for (unsigned i=0;i<8;++i) {
        if (bins[i]>=n) return h;
        const unsigned s=((bins[i]+n-1)%n)/4;
        symbols[i]=static_cast<std::uint16_t>(s^(s>>1));
    }
    const auto cw=deinterleave(symbols,sf-2,4);
    std::array<std::uint8_t,5> nibs{};
    for (unsigned i=0;i<5;++i) {
        const auto d=decode_nibble(cw[i],4);
        if (d.invalid) return h;
        nibs[i]=d.value; h.corrected|=d.corrected;
    }
    h.length=(static_cast<unsigned>(nibs[0])<<4)|nibs[1];
    h.parity=nibs[2]>>1; h.crc=(nibs[2]&1)!=0;
    const unsigned check=(static_cast<unsigned>(nibs[3])<<4)|nibs[4];
    if (!h.length || h.parity<1 || h.parity>4 || check!=header_checksum(static_cast<std::uint8_t>(h.length),nibs[2])) return h;
    const unsigned payload_width=sf-(ldro(sf,bandwidth)?2:0);
    const unsigned nibbles=2*(h.length+(h.crc?2:0));
    const unsigned carried=sf-7;
    const unsigned remaining=nibbles>carried?nibbles-carried:0;
    h.symbols=8+(remaining+payload_width-1)/payload_width*(4+h.parity);
    h.valid=true;
    return h;
}
} // namespace ovmesh::lora_detail
