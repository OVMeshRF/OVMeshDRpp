// SPDX-License-Identifier: GPL-3.0-or-later
// Generated receive-only samples, transient frames and bounded worker tests.
// No device access, files, real channel keys or operational observations.
#include "discovery_decoder.hpp"
#include "discovery_worker.hpp"
#include "ovmesh/meshtastic_presets.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>

namespace {
using namespace ovmesh;
using Complex = std::complex<float>;
constexpr double center = 865500000;
constexpr uint32_t rate = 2000000;
const std::array<DiscoveryChannelizer::Subband, 1> bands{{{center, center - 750000, center + 750000}}};
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct Fixture {
    std::vector<Complex> samples;
    std::vector<uint8_t> payload;
    PhyConfig config;
    LoRaDiscovery truth;
    size_t leading = 4096, frame_samples = 0;
};
Fixture fixture(PhyConfig config, int offset, uint32_t sample_rate = rate) {
    Fixture output; output.config = config;
    output.leading += static_cast<size_t>(std::abs(offset)) % 17;
    output.payload = {0x53, 0x19, 0x71, 0x04, 0x27, 0x98, 0x43, 0x61, 0x81, 0x22, 0x0e, 0x34};
    auto wave = modulate_lora(output.payload, config, sample_rate);
    output.frame_samples = wave.size();
    output.samples.resize(output.leading + wave.size() + 32768);
    Complex oscillator{1, 0};
    const auto step = std::polar(1.f, static_cast<float>(2 * std::numbers::pi * offset / sample_rate));
    for (size_t i = 0; i < wave.size(); ++i) {
        output.samples[output.leading + i] = wave[i] * oscillator * .25f;
        oscillator *= step;
        if ((i & 4095) == 4095) oscillator /= std::abs(oscillator);
    }
    const double symbol = static_cast<double>(1u << config.spreading_factor) * sample_rate / config.bandwidth_hz;
    output.truth = {center + offset, config.bandwidth_hz, config.spreading_factor,
        static_cast<double>(output.leading), output.leading + 10 * symbol, 1, 1, 1};
    return output;
}

std::vector<DiscoveryDecodedFrame> decode_fixture(const Fixture& input, bool automatic,
    DiscoveryDecoderStats& statistics, size_t block_size = 4096) {
    DiscoveryDecoderOptions options; options.enabled = true;
    DiscoveryDecoder decoder(0, bands, center - 740000, center + 740000, options);
    LoRaPreambleDiscovery detector(center);
    std::vector<DiscoveryDecodedFrame> frames;
    const auto receive = [&](DiscoveryDecodedFrame&& frame) { frames.push_back(std::move(frame)); };
    constexpr uint64_t anchor = 731;
    constexpr uint32_t stride = 4;
    const double symbol = static_cast<double>(1u << input.config.spreading_factor) * rate / input.config.bandwidth_hz;
    bool observed = false;
    for (size_t first = 0; first < input.samples.size(); first += block_size) {
        const auto values = std::span(input.samples).subspan(first, std::min(block_size, input.samples.size() - first));
        decoder.feed(values, first, anchor, stride, 3, receive);
        if (automatic) detector.feed(values, first, [&](const LoRaDiscovery& found) { decoder.observe(found, receive); });
        else if (!observed && first + values.size() >= input.truth.delimiter_sample + 2 * symbol) {
            observed = true; decoder.observe(input.truth, receive);
        }
    }
    statistics = decoder.stats();
    require(statistics.history_samples <= DiscoveryDecoder::maximum_history_samples,
        "Transient IQ history remains bounded while payload reception continues");
    decoder.reset();
    require(decoder.stats().active_decoders == 0 && decoder.stats().history_samples == 0,
        "Reset clears active decoders and replay history");
    for (const auto& frame : frames) {
        require(frame.segment_id == 3 && frame.subband_index == 0 &&
            frame.bandwidth_hz == input.config.bandwidth_hz && frame.spreading_factor == input.config.spreading_factor,
            "Decoded handoff preserves discovery settings and segment provenance");
        require(std::abs(frame.center_hz - input.truth.center_hz) < 100,
            "Decoder follows discovered off-center frequency without a preset slot");
        require(std::abs(frame.delimiter_input_sample - (anchor + input.truth.delimiter_sample * stride)) < symbol * stride / 8 &&
            std::abs(frame.first_input_sample - (anchor + input.leading * stride)) < symbol * stride * 2 &&
            std::abs(frame.end_input_sample - (anchor + (input.leading + input.frame_samples) * stride)) < symbol * stride / 4,
            "Frame times map to the original input clock with channelizer delay removed");
    }
    return frames;
}

void arbitrary_waveforms() {
    std::vector<std::tuple<std::string_view, PhyConfig, int>> cases;
    for (size_t i = 0; i < meshtastic::presets.size(); ++i) {
        const auto& preset = meshtastic::presets[i];
        cases.emplace_back(preset.name, PhyConfig{preset.bandwidth_hz, preset.spreading_factor,
            preset.coding_rate_denominator, 0x2b},
            (i % 2 ? -1 : 1) * (111733 + static_cast<int>(i) * 13719));
    }
    cases.emplace_back("Non-preset CR4/7", PhyConfig{500000, 7, 7, 0x2b}, 337119);
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto& [name, config, offset] = cases[i];
        const auto input = fixture(config, offset);
        DiscoveryDecoderStats stats;
        auto frames = decode_fixture(input, true, stats, i == 0 ? 997 : 4096);
        std::cout << name << " BW=" << config.bandwidth_hz << " SF=" << unsigned(config.spreading_factor)
            << " frames=" << frames.size() << " started=" << stats.started << " history misses=" << stats.history_misses
            << " preambles=" << stats.phy.preamble_candidates << " sync=" << stats.phy.sync_matches
            << " headers=" << stats.phy.headers_valid;
        if (!frames.empty()) std::cout << " CRC=" << frames[0].frame.payload_crc_valid
            << " CR=" << unsigned(frames[0].frame.coding_rate) << " CFO=" << frames[0].frame.frequency_error_hz;
        std::cout << '\n';
        require(frames.size() == 1 && frames[0].frame.header_valid && frames[0].frame.payload_crc_valid &&
            frames[0].frame.bytes == input.payload && frames[0].frame.coding_rate == config.coding_rate && stats.crc_valid == 1,
            "Discovered arbitrary-frequency LoRa must produce the exact CRC-valid synthetic frame");
    }
}

void failure_and_bounds() {
    DiscoveryDecoderOptions options; options.enabled = true;
    std::vector<Complex> quiet(DiscoveryDecoder::maximum_block);
    std::vector<DiscoveryDecodedFrame> frames;
    const auto receive = [&](DiscoveryDecodedFrame&& frame) { frames.push_back(std::move(frame)); };
    DiscoveryDecoder decoder(0, bands, center - 740000, center + 740000, options);
    for (uint64_t first = 0; first < 32768; first += quiet.size()) decoder.feed(quiet, first, 0, 1, 1, receive);
    LoRaDiscovery candidate{center - 200000, 125000, 7, 10000, 20000, 1, 1, 1};
    for (size_t i = 0; i <= DiscoveryDecoder::maximum_active; ++i) {
        candidate.center_hz = center - 200000 + i * 40000;
        decoder.observe(candidate, receive);
    }
    require(decoder.stats().started == DiscoveryDecoder::maximum_active && decoder.stats().active_limit_hits == 1,
        "Concurrent decoder saturation is bounded and reported");
    decoder.feed(quiet, 50000, 0, 1, 1, receive);
    require(decoder.stats().resets == 1 && decoder.stats().abandoned_decoders == DiscoveryDecoder::maximum_active && frames.empty(),
        "A gap abandons partial decoders instead of bridging missing IQ");
    decoder.observe(candidate, receive);
    require(decoder.stats().history_misses == 1, "A confirmation older than available history reports a replay miss");

    auto valid = fixture({125000, 8, 5, 0x2b}, 123719);
    const size_t symbol = (1u << valid.config.spreading_factor) * rate / valid.config.bandwidth_hz;
    const size_t payload_start = valid.leading + 12 * symbol + symbol / 4 + 8 * symbol;
    std::fill(valid.samples.begin() + static_cast<std::ptrdiff_t>(payload_start), valid.samples.end(), Complex{});
    DiscoveryDecoderStats bad_stats;
    auto bad_frames = decode_fixture(valid, false, bad_stats);
    require(bad_frames.size() == 1 && !bad_frames[0].frame.payload_crc_valid && bad_frames[0].frame.bytes.empty(),
        "CRC-failed frames expose PHY metadata without retaining failed frame bytes");

    options.exclusions.push_back({center + 123719, 125000, 8});
    DiscoveryDecoder excluded(0, bands, center - 740000, center + 740000, options);
    for (uint64_t first = 0; first < 65536; first += quiet.size()) excluded.feed(quiet, first, 0, 1, 1, receive);
    excluded.observe(valid.truth, receive);
    require(excluded.stats().excluded_candidates == 1 && excluded.stats().started == 0,
        "An existing configured RF lane excludes only its matching discovered settings");
    auto outside = valid.truth; outside.center_hz = center + 740000;
    excluded.observe(outside, receive);
    require(excluded.stats().outside_range_candidates == 1,
        "Partial-footprint edge candidates do not launch payload decoders");
    DiscoveryDecoder short_history(0, bands, center - 740000, center + 740000, options, 4096);
    short_history.feed(quiet, 0, 0, 1, 1, receive);
    short_history.feed(quiet, 4096, 0, 1, 1, receive);
    require(short_history.stats().history_samples == 4096, "Ring storage evicts only the oldest bounded IQ");
    bool rejected = false;
    try { short_history.feed(quiet, uint64_t{1} << 53, 0, 1, 1, receive); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Inexact input coordinates cannot produce false timestamps");
}

void reset_reacquisition() {
    DiscoveryDecoderOptions options; options.enabled = true;
    constexpr size_t capacity = 65536;
    DiscoveryDecoder decoder(0, bands, center - 740000, center + 740000, options, capacity);
    const auto input = fixture({500000, 7, 6, 0x2b}, 119731);
    std::vector<DiscoveryDecodedFrame> frames;
    const auto receive = [&](DiscoveryDecodedFrame&& frame) { frames.push_back(std::move(frame)); };
    const double symbol = 128. * rate / input.config.bandwidth_hz;
    const size_t confirmation_end = static_cast<size_t>(input.truth.delimiter_sample + 2 * symbol);
    auto feed_part = [&](size_t first, size_t last, uint64_t origin, uint64_t segment) {
        for (; first < last;) {
            const auto count = std::min(DiscoveryDecoder::maximum_block, last - first);
            decoder.feed(std::span(input.samples).subspan(first, count), origin + first, 731, 4, segment, receive);
            first += count;
        }
    };
    auto observed_at = [&](uint64_t origin) {
        auto truth = input.truth;
        truth.first_observed_upchirp_sample += origin;
        truth.delimiter_sample += origin;
        decoder.observe(truth, receive);
    };
    // Start near the ring boundary, launch a frame, then lose one sample.
    // Its remainder must not be decoded across the discontinuity.
    constexpr uint64_t first_origin = capacity - 2048;
    feed_part(0, confirmation_end, first_origin, 1);
    observed_at(first_origin);
    require(decoder.stats().active_decoders == 1, "Synthetic partial frame starts before gap");
    feed_part(confirmation_end, input.samples.size(), first_origin + 1, 1);
    require(frames.empty() && decoder.stats().abandoned_decoders == 1,
        "A wrapped replay ring cannot bridge even a one-sample gap");
    decoder.reset(); decoder.reset();
    require(decoder.stats().history_samples == 0, "Repeated reset leaves no replay history");
    // Reuse the same storage at a different nonzero coordinate and segment.
    constexpr uint64_t next_origin = 3 * capacity - 997;
    feed_part(0, confirmation_end, next_origin, 2);
    observed_at(next_origin);
    feed_part(confirmation_end, input.samples.size(), next_origin, 2);
    require(frames.size() == 1 && frames.front().frame.payload_crc_valid &&
        frames.front().frame.bytes == input.payload && frames.front().segment_id == 2,
        "After gap and repeated resets, reused wrapped storage reacquires the exact fresh frame");
}

void worker_dispatch() {
    const auto input = fixture({500000, 7, 6, 0x2b}, 119731, 8000000);
    DiscoveryDecoderOptions options; options.enabled = true;
    DiscoveryWorker worker(8000000, center, center - 740000, center + 740000, options);
    uint64_t accepted = 0;
    for (size_t first = 0; first < input.samples.size(); first += DiscoveryWorker::maximum_input_block) {
        const auto values = std::span(input.samples).subspan(first,
            std::min(DiscoveryWorker::maximum_input_block, input.samples.size() - first));
        require(worker.submit(values, first + 177), "Paced synthetic worker input is accepted");
        accepted += values.size();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (worker.snapshot().channelized_input_samples < accepted) {
            require(!worker.snapshot().failed && std::chrono::steady_clock::now() < deadline,
                "Bounded worker makes progress on the synthetic packet");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    worker.finish();
    auto decoded = worker.take_frames();
    const auto state = worker.snapshot();
    require(!state.failed && decoded.size() == 1 && decoded[0].frame.payload_crc_valid && decoded[0].frame.bytes == input.payload,
        "PFB, asynchronous discovery, replay dispatch and PHY decode work together");
    require(state.automatic_decoder.completed == 1 && state.automatic_decoder.active_decoders == 0 &&
        state.automatic_decoder.history_samples == 0 && worker.take_frames().empty(),
        "Finished worker releases IQ and drains frame ownership exactly once");
}
}

int main() {
    static_assert(!std::is_copy_constructible_v<DiscoveryDecodedFrame>);
    static_assert(std::is_nothrow_move_constructible_v<DiscoveryDecodedFrame>);
    try {
        arbitrary_waveforms(); failure_and_bounds(); reset_reacquisition(); worker_dispatch();
        std::cout << "Automatic discovery decoder checks passed (synthetic, no hardware)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Automatic discovery decoder checks failed: " << error.what() << '\n';
        return 1;
    }
}
