// SPDX-License-Identifier: GPL-3.0-or-later
// Paced, bounded, offline source for the experimental asynchronous discovery
// worker. It never opens hardware, reads recordings or writes sample data.
// Build against the same optimized discovery and spectrum objects as the app.
// Example: discovery-worker-capacity --seconds 3 --scenario all --span-mhz all
// Add --spectrum to execute the actual SpectrumProcessor in the submitting
// caller. Storage, GUI, USB and physical RF effects remain outside this test.
#include "discovery_observations.hpp"
#include "discovery_worker.hpp"
#include "lora_discovery_fixtures.hpp"
#include "spectrum.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
namespace f = lora_discovery_fixtures;
using Worker = ovmesh::DiscoveryWorker;
using Clock = std::chrono::steady_clock;
constexpr uint32_t sample_rate = 16000000;
constexpr uint64_t source_origin = 8191;
constexpr double center_hz = 907500000;

struct Options {
    unsigned seconds = 3;
    std::string scenario = "all", span = "all";
    bool spectrum = false, decode = true, help = false;
};

Options options(int argc, char** argv) {
    Options out;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help") { out.help = true; continue; }
        if (arg == "--spectrum") { out.spectrum = true; continue; }
        if (arg == "--no-decode") { out.decode = false; continue; }
        if (arg != "--seconds" && arg != "--scenario" && arg != "--span-mhz")
            throw std::invalid_argument("Unknown diagnostic option; use --help");
        if (++i == argc) throw std::invalid_argument("Missing diagnostic option value");
        const std::string_view value(argv[i]);
        if (arg == "--seconds") {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), out.seconds);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                out.seconds < 1 || out.seconds > 30)
                throw std::invalid_argument("--seconds must be an integer from 1 to 30");
        } else if (arg == "--scenario") out.scenario = value;
        else out.span = value;
    }
    if (out.scenario != "all" && out.scenario != "quiet" && out.scenario != "noise" &&
        out.scenario != "cw" && out.scenario != "lora" && out.scenario != "packets")
        throw std::invalid_argument("--scenario must be all, quiet, noise, cw, lora or packets");
    if (out.span != "all" && out.span != "12" && out.span != "12.8" && out.span != "5")
        throw std::invalid_argument("--span-mhz must be all, 12.8, 12 or 5");
    return out;
}

struct Source {
    // One second of unique noise avoids artificial short-block repetition at a
    // LoRa symbol lag. This period is reused in each second, explicitly; source
    // synthesis is completed before timing begins. The retained period is
    // 122 MiB; one bounded fixture is constructed at a time and then discarded.
    std::vector<f::Complex> period;
    std::vector<f::FrameTruth> truth;
    // Only generated packet fixtures have byte expectations; no keys or identities.
    std::vector<std::array<uint8_t, 12>> payloads;
    std::vector<uint8_t> coding_rates;
};

Source make_source(const std::string& scenario) {
    Source out;
    out.period.resize(sample_rate);
    if (scenario != "quiet") f::add_noise(out.period, .02, 0x973108569ULL);
    if (scenario == "cw") {
        f::add_cw(out.period, sample_rate, 0, .25);
        f::add_cw(out.period, sample_rate, 1337321, .15);
    }
    if (scenario == "lora") {
        for (unsigned i = 0; i < 2; ++i) {
            f::PreambleSpec spec;
            spec.sample_rate_hz = sample_rate;
            spec.bandwidth_hz = i ? 500000 : 250000;
            spec.spreading_factor = 11;
            spec.center_offset_hz = i ? 1237391.75 : -1491273;
            spec.cfo_hz = i ? -873.5 : 927.5;
            spec.leading_samples = i ? 2400000 : 1920000;
            spec.fractional_start_samples = i ? .375 : .625;
            spec.amplitude = .08;
            spec.sync_word = i ? 0x34 : 0x12;
            const auto fixture = f::make_preamble(spec);
            if (fixture.samples.size() > out.period.size())
                throw std::logic_error("Independent fixture exceeds source period");
            for (size_t j = 0; j < fixture.samples.size(); ++j) out.period[j] += fixture.samples[j];
            out.truth.push_back(fixture.truth);
        }
    }
    if (scenario == "packets") {
        for (unsigned i = 0; i < 2; ++i) {
            const ovmesh::PhyConfig phy{i ? 500000u : 250000u, 11,
                static_cast<uint8_t>(i ? 8 : 5), 0x2b};
            const std::array<uint8_t, 12> payload{0x53, 0x19, 0x71, 0x04, 0x27, 0x98,
                0x43, 0x61, 0x81, 0x22, 0x0e, static_cast<uint8_t>(0x34 + i)};
            // This complete-frame workload deliberately uses the application
            // encoder. The independent analytic preamble workload above remains
            // separate; this is capacity evidence, not independent codec proof.
            const auto wave = ovmesh::modulate_lora(payload, phy, sample_rate);
            const size_t leading = i ? 2400007 : 1920013;
            if (wave.size() + leading + sample_rate / 20 > out.period.size())
                throw std::logic_error("Complete packet and trailing guard exceed source period");
            const double offset = i ? 1236518.25 : -1490345.5;
            f::Complex oscillator{1, 0};
            const auto step = std::polar(1.f, static_cast<float>(2 * std::numbers::pi * offset / sample_rate));
            for (size_t j = 0; j < wave.size(); ++j) {
                out.period[leading + j] += .08f * wave[j] * oscillator;
                oscillator *= step;
                if ((j & 4095) == 4095) oscillator /= std::abs(oscillator);
            }
            const double symbol = static_cast<double>(1u << phy.spreading_factor) * sample_rate / phy.bandwidth_hz;
            f::FrameTruth truth;
            truth.sample_rate_hz = sample_rate;
            truth.start_sample = static_cast<double>(leading);
            truth.preamble_end_sample = leading + 8 * symbol;
            truth.sync_end_sample = leading + 10 * symbol;
            truth.sfd_end_sample = leading + 12.25 * symbol;
            truth.first_sample = leading;
            truth.end_sample = leading + wave.size();
            truth.symbol_samples = symbol;
            truth.symbol_seconds = symbol / sample_rate;
            truth.configured_center_offset_hz = truth.received_center_offset_hz = offset;
            truth.lower_offset_hz = offset - phy.bandwidth_hz / 2.;
            truth.upper_offset_hz = offset + phy.bandwidth_hz / 2.;
            truth.bandwidth_hz = phy.bandwidth_hz;
            truth.spreading_factor = phy.spreading_factor;
            truth.preamble_symbols = 8;
            truth.sync_word = phy.sync_word;
            out.truth.push_back(truth);
            out.payloads.push_back(payload);
            out.coding_rates.push_back(phy.coding_rate);
        }
    }
    return out;
}

double seconds(Clock::duration duration) { return std::chrono::duration<double>(duration).count(); }

void run(const Source& source, const std::string& scenario, double span_mhz,
         const Options& config) {
    const auto span_hz = static_cast<uint32_t>(std::llround(span_mhz * 1000000));
    ovmesh::DiscoveryDecoderOptions decoder; decoder.enabled = config.decode;
    Worker worker(sample_rate, center_hz, center_hz - span_hz / 2., center_hz + span_hz / 2., decoder);
    const size_t subbands = worker.snapshot().subbands.size();
    ovmesh::DiscoveryObservations observations(sample_rate, subbands);
    std::unique_ptr<ovmesh::SpectrumProcessor> spectrum;
    if (config.spectrum) spectrum = std::make_unique<ovmesh::SpectrumProcessor>(
        static_cast<uint64_t>(center_hz), sample_rate, span_hz, -55.f);
    uint64_t tiles = 0, spectrum_frames = 0, spectrum_events = 0;
    const auto tile = [&](ovmesh::SpectrumTile value) { ++tiles; spectrum_frames += value.frame_count; };
    const auto event = [&](ovmesh::SpectrumEvent) { ++spectrum_events; };
    uint64_t raw_results = 0, boundary_results = 0, merged_results = 0;
    uint64_t received_frames = 0, crc_valid_frames = 0, unexpected_frames = 0, repeated_frames = 0;
    uint64_t payload_mismatches = 0;
    std::vector<bool> matched_packets(source.payloads.size() * config.seconds);
    const auto matching_truth = [&](uint32_t bandwidth, unsigned sf, double center, double delimiter,
                                    unsigned period, size_t index) {
        const auto& truth = source.truth[index];
        return bandwidth == truth.bandwidth_hz && sf == truth.spreading_factor &&
            std::abs(center - (center_hz + truth.received_center_offset_hz)) <=
                2. * truth.bandwidth_hz / (1u << truth.spreading_factor) &&
            std::abs(delimiter - (static_cast<double>(source_origin) +
                period * static_cast<double>(sample_rate) + truth.sync_end_sample)) <=
                2. * sample_rate / truth.bandwidth_hz;
    };
    uint64_t gap_records = 0, all_subband_gap_samples = 0, individual_subband_gap_samples = 0;
    const auto collect = [&] {
        for (const auto& decoded : worker.take_frames()) {
            ++received_frames;
            const bool valid = decoded.frame.header_valid && decoded.frame.payload_crc_present &&
                decoded.frame.payload_crc_valid;
            if (valid) ++crc_valid_frames;
            bool matched = false;
            for (unsigned period = 0; period < config.seconds && !matched; ++period) {
                for (size_t i = 0; i < source.payloads.size() && !matched; ++i) {
                    if (!matching_truth(decoded.bandwidth_hz, decoded.spreading_factor,
                        decoded.center_hz, decoded.delimiter_input_sample, period, i)) continue;
                    matched = true;
                    if (!valid || decoded.frame.coding_rate != source.coding_rates[i] ||
                        !std::equal(decoded.frame.bytes.begin(), decoded.frame.bytes.end(),
                            source.payloads[i].begin(), source.payloads[i].end())) {
                        ++payload_mismatches;
                        continue;
                    }
                    const size_t index = period * source.payloads.size() + i;
                    if (matched_packets[index]) ++repeated_frames;
                    matched_packets[index] = true;
                }
            }
            if (!matched) ++unexpected_frames;
            // Move-only handoff destruction erases transient bytes here. No
            // received payload is printed or copied into retained metadata.
        }
        for (const auto& result : worker.take_results()) {
            ++raw_results;
            if (!result.complete_in_requested_range) ++boundary_results;
            if (observations.observe(result.waveform, result.subband_index,
                                     result.first_input_anchor, result.input_sample_stride).merged)
                ++merged_results;
        }
        for (const auto& gap : worker.take_gaps()) {
            ++gap_records;
            // These are records, not a temporal union: processing-failure source
            // and individual subband records can overlap. Keep them separate.
            if (gap.subband_index == Worker::all_subbands)
                all_subband_gap_samples += gap.end_input_sample - gap.first_input_sample;
            else individual_subband_gap_samples += gap.end_input_sample - gap.first_input_sample;
        }
    };

    const uint64_t total = static_cast<uint64_t>(config.seconds) * sample_rate;
    uint64_t accepted = 0, rejected = 0, blocks = 0, late_blocks = 0;
    double maximum_lateness = 0, maximum_submit = 0, submit_seconds = 0, spectrum_seconds = 0;
    const auto cpu_start = std::clock();
    const auto start = Clock::now();
    for (uint64_t first = 0; first < total;) {
        const size_t position = static_cast<size_t>(first % sample_rate);
        const auto count = static_cast<size_t>(std::min<uint64_t>(
            {Worker::maximum_input_block, sample_rate - position, total - first}));
        const auto input = std::span(source.period).subspan(position, count);
        // Model a USB callback delivered after the last sample of its block was
        // acquired. If the caller falls behind, preserve that lateness as evidence
        // instead of silently slowing the nominal source clock.
        const auto due = start + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(static_cast<double>(first + count) / sample_rate));
        std::this_thread::sleep_until(due);
        const auto delivery = Clock::now();
        const double late = std::max(0., seconds(delivery - due));
        maximum_lateness = std::max(maximum_lateness, late);
        if (late > static_cast<double>(count) / sample_rate) ++late_blocks;
        const auto submit_start = Clock::now();
        if (worker.submit(input, source_origin + first)) accepted += count;
        else rejected += count;
        const double submitted = seconds(Clock::now() - submit_start);
        submit_seconds += submitted;
        maximum_submit = std::max(maximum_submit, submitted);
        if (spectrum) {
            const auto measurement_start = Clock::now();
            // Full-rate spectrum receives every sample even if discovery drops.
            spectrum->feed(input, source_origin + first, tile, event);
            spectrum_seconds += seconds(Clock::now() - measurement_start);
        }
        collect();
        first += count;
        ++blocks;
    }
    const auto delivery_end = Clock::now();
    const auto before_drain = worker.snapshot();
    uint64_t least_processed = total;
    for (const auto& band : before_drain.subbands)
        least_processed = std::min(least_processed, band.last_processed_input_sample > source_origin ?
            std::min(total, band.last_processed_input_sample - source_origin) : uint64_t{0});
    const auto drain_start = Clock::now();
    worker.finish();
    const double drain_seconds = seconds(Clock::now() - drain_start);
    collect();
    if (spectrum) {
        const auto measurement_start = Clock::now();
        spectrum->finish(tile, event);
        spectrum_seconds += seconds(Clock::now() - measurement_start);
    }
    const double total_wall = seconds(Clock::now() - start);
    const double cpu_seconds = static_cast<double>(std::clock() - cpu_start) / CLOCKS_PER_SEC;
    const auto state = worker.snapshot();
    if (accepted != state.accepted_input_samples || rejected != state.rejected_input_samples ||
        accepted + rejected != total ||
        state.channelized_input_samples + state.abandoned_input_samples != accepted)
        throw std::runtime_error("Worker input-accounting invariant failed");

    uint64_t processed = 0, abandoned = 0, ffts = 0, windows = 0, candidates = 0, tracks = 0;
    uint64_t outside = 0, discovered = 0;
    for (const auto& band : state.subbands) {
        processed += band.processed_output_samples; abandoned += band.abandoned_output_samples;
        ffts += band.fft_searches; windows += band.windows;
        candidates += band.candidate_limit_hits; tracks += band.track_limit_hits;
        outside += band.outside_range_candidates; discovered += band.discoveries;
    }
    if (raw_results + state.result_overflows != discovered)
        throw std::runtime_error("Worker result-accounting invariant failed");
    if (received_frames + state.automatic_decoder.frame_overflows != state.automatic_decoder.completed)
        throw std::runtime_error("Worker frame-accounting invariant failed");
    const size_t expected = source.truth.size() * config.seconds;
    std::vector<bool> matched(expected);
    size_t unexpected = 0, repeated_match = 0;
    double maximum_center_error = 0, maximum_delimiter_error = 0;
    for (const auto& found : observations.observations()) {
        bool match = false;
        for (unsigned period = 0; period < config.seconds && !match; ++period) {
            for (size_t i = 0; i < source.truth.size() && !match; ++i) {
                const auto& truth = source.truth[i];
                if (found.bandwidth_hz != truth.bandwidth_hz ||
                    found.spreading_factor != truth.spreading_factor) continue;
                const double frequency_error = std::abs(found.received_center_hz -
                    (center_hz + truth.received_center_offset_hz));
                const double time_error = std::abs(found.delimiter_input_sample -
                    (static_cast<double>(source_origin) + period * static_cast<double>(sample_rate) + truth.sync_end_sample));
                if (frequency_error > 2. * truth.bandwidth_hz / (1u << truth.spreading_factor) ||
                    time_error > 2. * sample_rate / truth.bandwidth_hz) continue;
                const size_t index = period * source.truth.size() + i;
                if (matched[index]) ++repeated_match;
                matched[index] = true;
                match = true;
                maximum_center_error = std::max(maximum_center_error, frequency_error);
                maximum_delimiter_error = std::max(maximum_delimiter_error, time_error);
            }
        }
        if (!match) ++unexpected;
    }

    std::cout << std::setprecision(9)
        << "scenario=" << scenario << " span_mhz=" << span_mhz << " subbands=" << subbands
        << " spectrum=" << config.spectrum << " automatic_decode=" << config.decode
        << " detector_workers=" << Worker::detector_workers << " source_slots=" << Worker::source_capacity
        << " nominal_input_seconds=" << config.seconds
        << " delivered_wall_seconds=" << seconds(delivery_end - start)
        << " drain_seconds=" << drain_seconds << " total_wall_seconds=" << total_wall
        << " process_cpu_seconds=" << cpu_seconds << " blocks=" << blocks
        << " max_delivery_lateness_ms=" << maximum_lateness * 1000 << " late_beyond_block=" << late_blocks
        << " submit_seconds=" << submit_seconds << " max_submit_ms=" << maximum_submit * 1000
        << " accepted_samples=" << accepted << " rejected_samples=" << rejected
        << " rejected_percent=" << 100. * static_cast<double>(rejected) / static_cast<double>(total)
        << " channelized_samples=" << state.channelized_input_samples
        << " abandoned_source_samples=" << state.abandoned_input_samples
        << " source_gap_samples=" << state.source_gap_input_samples
        << " source_drop_blocks=" << state.source_queue_drops
        << " source_queue_high_water=" << state.source_queue_high_water
        << " source_backlog_at_end=" << before_drain.accepted_input_samples - before_drain.channelized_input_samples
        << " slowest_subband_lag_at_end_seconds=" << static_cast<double>(total - least_processed) / sample_rate
        << " processed_output_samples=" << processed << " abandoned_output_samples=" << abandoned
        << " stream_resets=" << state.stream_resets << " windows=" << windows << " ffts=" << ffts
        << " candidate_limit_hits=" << candidates << " track_limit_hits=" << tracks
        << " raw_results=" << raw_results << " cross_subband_merges=" << merged_results
        << " retained_observations=" << observations.observations().size()
        << " observation_evictions=" << observations.eviction_count()
        << " result_overflows=" << state.result_overflows << " boundary_results=" << boundary_results
        << " guard_only_candidates=" << outside << " expected_preambles=" << expected
        << " matched_preambles=" << std::count(matched.begin(), matched.end(), true)
        << " unexpected_observations=" << unexpected << " repeated_truth_matches=" << repeated_match
        << " max_center_error_hz=" << maximum_center_error
        << " max_delimiter_error_input_samples=" << maximum_delimiter_error
        << " gap_records=" << gap_records << " gap_overflows=" << state.gap_overflows
        << " all_subband_gap_record_samples=" << all_subband_gap_samples
        << " individual_subband_gap_record_samples=" << individual_subband_gap_samples
        << " spectrum_seconds=" << spectrum_seconds << " spectrum_tiles=" << tiles
        << " spectrum_frames=" << spectrum_frames << " spectrum_events=" << spectrum_events
        << " spectrum_partial_samples=" << (spectrum ? spectrum->dropped_partial_samples() : 0)
        << " decoder_candidates=" << state.automatic_decoder.candidates
        << " decoder_started=" << state.automatic_decoder.started
        << " decoder_active_at_end=" << before_drain.automatic_decoder.active_decoders
        << " decoder_active_after_drain=" << state.automatic_decoder.active_decoders
        << " decoder_completed=" << state.automatic_decoder.completed
        << " decoder_crc_valid=" << state.automatic_decoder.crc_valid
        << " decoder_preamble_candidates=" << state.automatic_decoder.phy.preamble_candidates
        << " decoder_sync_matches=" << state.automatic_decoder.phy.sync_matches
        << " decoder_sync_rejections=" << state.automatic_decoder.phy.sync_rejections
        << " decoder_valid_headers=" << state.automatic_decoder.phy.headers_valid
        << " decoder_failed_headers=" << state.automatic_decoder.phy.headers_failed
        << " decoder_history_misses=" << state.automatic_decoder.history_misses
        << " decoder_active_limit_hits=" << state.automatic_decoder.active_limit_hits
        << " decoder_frame_overflows=" << state.automatic_decoder.frame_overflows
        << " decoder_timeouts=" << state.automatic_decoder.timeouts
        << " decoder_resets=" << state.automatic_decoder.resets
        << " decoder_abandoned=" << state.automatic_decoder.abandoned_decoders
        << " received_frames=" << received_frames << " crc_valid_frames=" << crc_valid_frames
        << " expected_packets=" << matched_packets.size()
        << " exact_payload_packets=" << std::count(matched_packets.begin(), matched_packets.end(), true)
        << " payload_mismatches=" << payload_mismatches << " unexpected_frames=" << unexpected_frames
        << " repeated_packet_matches=" << repeated_frames
        << " failed=" << state.failed << '\n';
    std::cout << "detector_queue_high_water=";
    for (size_t i = 0; i < state.detector_queue_high_water.size(); ++i)
        std::cout << (i ? "," : "") << state.detector_queue_high_water[i];
    std::cout << '\n';
    // A small bounded row per PFB subband exposes uneven waveform-search work
    // without imposing an assumption about fixed detector-thread ownership.
    for (size_t i = 0; i < state.subbands.size(); ++i) {
        const auto& band = state.subbands[i];
        std::cout << "subband=" << i << " center_hz=" << band.band.center_hz
            << " processed_samples=" << band.processed_output_samples
            << " windows=" << band.windows << " ffts=" << band.fft_searches
            << " candidate_limit_hits=" << band.candidate_limit_hits
            << " discoveries=" << band.discoveries << " resets=" << band.resets_after_gap << '\n';
    }
    if (state.failed) std::cout << "fault=" << state.fault << '\n';
    size_t printed = 0;
    for (const auto& found : observations.observations()) {
        if (printed++ == 64) break;
        std::cout << "observation id=" << found.id << " center_hz=" << found.received_center_hz
                  << " bandwidth_hz=" << found.bandwidth_hz << " sf=" << found.spreading_factor
                  << " delimiter_input_sample=" << found.delimiter_input_sample
                  << " contributing_subbands=" << found.contributing_subbands.size() << '\n';
    }
    if (observations.observations().size() > 64)
        std::cout << "additional_observations_not_printed=" << observations.observations().size() - 64 << '\n';
    std::cout.flush();
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto config = options(argc, argv);
        if (config.help) {
            std::cout << "Usage: discovery-worker-capacity [--seconds 1..30] "
                         "[--scenario all|quiet|noise|cw|lora|packets] [--span-mhz all|12.8|12|5] "
                         "[--spectrum] [--no-decode]\n"
                         "Paced 16 MS/s, 1-second generated source period; automatic decode is enabled by default.\n"
                         "No hardware. No sample files. Final drain waits for the finite accepted queue.\n";
            return 0;
        }
        std::cout << "Offline paced experiment. Includes PFB, asynchronous detectors, bounded queues and "
                     "metadata collection; optional actual spectrum processing. Excludes storage, GUI, USB "
                     "and physical RF effects. Source generated before timing, 1-second period repeated. "
                     "Scenario lora uses independent analytic preambles without encoded payloads. Scenario packets "
                     "uses complete generated PHY frames and reports exact-payload/CRC counts without printing bytes. "
                     "Neither establishes sensitivity, mesh identity or real-RF capacity.\n";
        for (const std::string scenario : {"quiet", "noise", "cw", "lora", "packets"}) {
            if (config.scenario != "all" && config.scenario != scenario) continue;
            const auto source = make_source(scenario);
            for (const std::string_view span : {"12.8", "12", "5"}) {
                if (config.span != "all" && config.span != span) continue;
                run(source, scenario, std::stod(std::string(span)), config);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "Capacity diagnostic failed: " << error.what() << '\n';
        return 1;
    }
}
