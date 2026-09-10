// SPDX-License-Identifier: GPL-3.0-or-later
#include "lora_discovery.hpp"
#include "lora_discovery_fixtures.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace f = lora_discovery_fixtures;
using ovmesh::LoRaDiscovery;
void require(bool okay, const char* text) { if (!okay) throw std::runtime_error(text); }
std::vector<LoRaDiscovery> detect(const std::vector<f::Complex>& samples) {
    ovmesh::LoRaPreambleDiscovery detector(907500000);
    std::vector<LoRaDiscovery> results;
    for (size_t i = 0; i < samples.size(); i += 4093) {
        detector.feed(std::span(samples).subspan(i, std::min(size_t{4093}, samples.size() - i)), i,
                      [&](const auto& d) { results.push_back(d); });
    }
    return results;
}
void exact_coordinate_bounds() {
    constexpr uint64_t limit = uint64_t{1} << 53;
    ovmesh::LoRaPreambleDiscovery detector(907500000);
    std::array<f::Complex, 2> samples{{{.1f, .2f}, {.2f, .1f}}};
    unsigned callbacks = 0;
    const auto observed = [&](const LoRaDiscovery&) { ++callbacks; };
    detector.feed({}, limit, observed);
    require(detector.stats().samples == 0 && callbacks == 0, "Exact-bound empty feed is accepted without processing");
    const auto rejected = [&](std::span<const f::Complex> input, uint64_t first) {
        const auto before = detector.stats();
        bool invalid = false;
        try { detector.feed(input, first, observed); }
        catch (const std::invalid_argument&) { invalid = true; }
        require(invalid, "Inexact/crossing sample coordinate must be rejected");
        const auto after = detector.stats();
        require(after.samples == before.samples && after.resets == before.resets &&
                after.windows == before.windows && after.fft_searches == before.fft_searches && callbacks == 0,
                "Coordinate rejection occurs before state reset, sample processing or callback");
    };
    rejected({}, limit + 1);
    rejected(std::span(samples).first(1), limit);
    rejected(samples, limit - 1);
    rejected({}, std::numeric_limits<uint64_t>::max());
    rejected(std::span(samples).first(1), std::numeric_limits<uint64_t>::max() - 1);
    rejected(samples, std::numeric_limits<uint64_t>::max());
    detector.feed(std::span(samples).first(1), limit - 1, observed);
    const auto last = detector.stats();
    detector.feed({}, limit, observed);
    require(last.samples == 1 && detector.stats().samples == 1 && detector.stats().resets == last.resets,
            "A final representable sample and its exact-bound exclusive end remain supported");
}
int main() {
    try {
        exact_coordinate_bounds();
        unsigned index = 0;
        for (uint32_t bw : {125000u, 250000u, 500000u}) for (unsigned sf = 7; sf <= 12; ++sf) {
            f::PreambleSpec spec;
            spec.bandwidth_hz = bw; spec.spreading_factor = sf;
            spec.center_offset_hz = -412347. + index * 43719.;
            spec.cfo_hz = 973.25; spec.fractional_start_samples = .37;
            spec.leading_samples = 1037 + 331 * index;
            spec.noise_rms = .02;
            const auto fixture = f::make_preamble(spec);
            const auto found = detect(fixture.samples);
            std::cout << "BW=" << bw << " SF=" << sf << " center=" << fixture.truth.received_center_offset_hz << " detections=" << found.size() << '\n';
            for (const auto& d : found) std::cout << "  " << d.bandwidth_hz << " SF" << d.spreading_factor << " center=" << d.center_hz - 907500000 << " up=" << d.up_match << " down=" << d.down_match << '\n';
            require(found.size() == 1, "Expected one blind preamble identification, without duplicate or wrong hypotheses");
            require(found[0].bandwidth_hz == bw && found[0].spreading_factor == sf, "Wrong inferred LoRa bandwidth/SF");
            require(std::abs(found[0].center_hz - 907500000 - fixture.truth.received_center_offset_hz) < 2e6 / fixture.truth.symbol_samples,
                    "Joint center estimate exceeds one dechirped FFT bin");
            require(found[0].first_observed_upchirp_sample >= fixture.truth.start_sample - 2e6 * 2 / bw &&
                    found[0].first_observed_upchirp_sample < fixture.truth.preamble_end_sample,
                    "Observed preamble evidence must fall within the actual source preamble, allowing two chips for estimation");
            ++index;
        }
        std::vector<f::Complex> noise(600000);
        f::add_noise(noise, .2, 9238);
        require(detect(noise).empty(), "Noise incorrectly identified as LoRa");
        f::add_cw(noise, 2000000, 0, .5);
        f::add_cw(noise, 2000000, 271923, .2);
        require(detect(noise).empty(), "CW incorrectly identified as LoRa");
        std::cout << "Independent blind BW/SF/center and negative tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
