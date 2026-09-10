// SPDX-License-Identifier: GPL-3.0-or-later
// Offline synthetic capacity diagnostic. No hardware or file input/output.
#include "discovery_channelizer.hpp"
#include "lora_discovery.hpp"
#include "lora_discovery_fixtures.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
namespace f = lora_discovery_fixtures;
using Clock = std::chrono::steady_clock;
constexpr double receiver_center = 907500000;
constexpr uint32_t input_rate = 16000000;

void measure(const std::string& label, std::vector<f::Complex> input) {
    ovmesh::DiscoveryChannelizer bank(input_rate, receiver_center,
                                    receiver_center - 6000000, receiver_center + 6000000);
    std::vector<std::unique_ptr<ovmesh::LoRaPreambleDiscovery>> detectors;
    std::vector<uint64_t> next(bank.subbands().size());
    for (const auto& subband : bank.subbands())
        detectors.push_back(std::make_unique<ovmesh::LoRaPreambleDiscovery>(subband.center_hz));
    uint64_t confirmations = 0;
    double detector_seconds = 0;
    const auto start = Clock::now();
    for (size_t offset = 0; offset < input.size(); offset += 65536) {
        bank.feed(std::span(input).subspan(offset, std::min(size_t{65536}, input.size() - offset)), offset,
            [&](const auto& block) {
                const auto detector_start = Clock::now();
                const auto i = block.subband_index;
                detectors[i]->feed(block.values, next[i], [&](const auto&) { ++confirmations; });
                next[i] += block.values.size();
                detector_seconds += std::chrono::duration<double>(Clock::now() - detector_start).count();
            });
    }
    const double wall = std::chrono::duration<double>(Clock::now() - start).count();
    uint64_t windows = 0, ffts = 0;
    for (const auto& detector : detectors) {
        windows += detector->stats().windows;
        ffts += detector->stats().fft_searches;
    }
    const double seconds = static_cast<double>(input.size()) / input_rate;
    std::cout << std::setprecision(8) << label << " input_seconds=" << seconds
              << " wall_seconds=" << wall << " channelizer_seconds=" << wall - detector_seconds
              << " detector_seconds=" << detector_seconds << " realtime_ratio=" << seconds / wall
              << " subbands=" << detectors.size() << " windows=" << windows << " ffts=" << ffts
              << " raw_confirmations=" << confirmations << '\n';
}
}

int main(int argc, char** argv) {
    try {
        const std::string mode = argc == 2 ? argv[1] : "all";
        if (argc > 2 || (mode != "all" && mode != "noise" && mode != "cw" && mode != "fixture"))
            throw std::invalid_argument("Usage: discovery-capacity [all|noise|cw|fixture]; synthetic, no USB");
        std::cout << "Offline serial experiment; excludes spectrum, recording and GUI costs. "
                     "Raw confirmations may duplicate across overlapping subbands.\n";
        if (mode == "all" || mode == "noise" || mode == "cw") {
            std::vector<f::Complex> noise(8000000); // 0.5 s, 64 MiB, generated only
            f::add_noise(noise, .2, 391);
            if (mode == "all" || mode == "noise") measure("noise", noise);
            if (mode == "all" || mode == "cw") {
                f::add_cw(noise, input_rate, 0, .5);
                measure("noise_plus_CW", std::move(noise));
            }
        }
        if (mode == "all" || mode == "fixture") {
            f::PreambleSpec spec;
            spec.sample_rate_hz = input_rate;
            spec.center_offset_hz = -3876123;
            spec.cfo_hz = 927.5;
            spec.bandwidth_hz = 250000;
            spec.spreading_factor = 11;
            spec.leading_samples = 17363;
            spec.noise_rms = .02;
            auto fixture = f::make_preamble(spec);
            measure("off_grid_LoRa_250_SF11", std::move(fixture.samples));
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
