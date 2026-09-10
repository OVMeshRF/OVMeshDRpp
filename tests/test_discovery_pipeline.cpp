// SPDX-License-Identifier: GPL-3.0-or-later
// Source-generated RF only. Combines the experimental channelizer and detector;
// signal settings are supplied to the independent fixture, never to a detector.
#include "discovery_channelizer.hpp"
#include "discovery_observations.hpp"
#include "lora_discovery.hpp"
#include "lora_discovery_fixtures.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
namespace f = lora_discovery_fixtures;
using Bank = ovmesh::DiscoveryChannelizer;
constexpr double receiver_center = 907500000;
constexpr std::uint64_t input_origin = 99991;

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

struct Case {
    const char* name;
    std::uint32_t input_rate, bandwidth;
    unsigned sf;
    double offset, cfo, lower, upper;
};

struct Detection {
    ovmesh::LoRaDiscovery value;
    std::size_t subband;
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

PipelineResult run_pipeline(const Case& test, const f::Fixture& fixture) {
    Bank bank(test.input_rate, receiver_center, receiver_center + test.lower,
              receiver_center + test.upper);
    std::vector<Stream> streams;
    for (const auto& subband : bank.subbands()) {
        Stream stream;
        stream.detector = std::make_unique<ovmesh::LoRaPreambleDiscovery>(subband.center_hz);
        streams.push_back(std::move(stream));
    }
    PipelineResult result;
    ovmesh::DiscoveryObservations observations(test.input_rate, streams.size());
    auto deliver = [&](const Bank::Samples& samples) {
        auto& stream = streams.at(samples.subband_index);
        require(samples.input_sample_stride == test.input_rate / Bank::output_sample_rate,
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
            result.raw.push_back({discovery, samples.subband_index,
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

void check_case(const Case& test) {
    f::PreambleSpec spec;
    spec.sample_rate_hz = test.input_rate;
    spec.bandwidth_hz = test.bandwidth; spec.spreading_factor = test.sf;
    spec.center_offset_hz = test.offset; spec.cfo_hz = test.cfo;
    spec.leading_samples = 4097; spec.fractional_start_samples = .37;
    spec.trailing_samples = 4096; spec.noise_rms = .01;
    const auto fixture = f::make_preamble(spec);
    require(fixture.truth.lower_offset_hz >= test.lower && fixture.truth.upper_offset_hz <= test.upper,
            "Test waveform lies outside the requested survey range");
    const auto started = std::chrono::steady_clock::now();
    const auto result = run_pipeline(test, fixture);
    const auto& found = result.raw;
    const auto wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const double expected_center = receiver_center + fixture.truth.received_center_offset_hz;
    const double expected_delimiter = static_cast<double>(input_origin) + fixture.truth.sync_end_sample;
    const double frequency_tolerance = 1 / fixture.truth.symbol_seconds;
    double maximum_time_error = 0;
    for (const auto& detection : found) {
        const auto& value = detection.value;
        require(value.bandwidth_hz == test.bandwidth && value.spreading_factor == test.sf,
                "Channelized preamble generated an incorrect BW/SF hypothesis");
        require(std::abs(value.center_hz - expected_center) < frequency_tolerance,
                "Channelized RF center estimate exceeds one dechirped FFT bin");
        const double time_error = std::abs(detection.delimiter_input_sample - expected_delimiter);
        maximum_time_error = std::max(maximum_time_error, time_error);
        require(time_error < 1,
                "Fractional delimiter mapping exceeds one original input sample");
        require(std::abs(detection.delimiter_input_sample - std::round(detection.delimiter_input_sample)) > 1e-5,
                "Fractional delimiter estimate was rounded to an integer input coordinate");
        require(detection.observed_up_input_sample >= static_cast<double>(input_origin) + fixture.truth.start_sample &&
                detection.observed_up_input_sample < static_cast<double>(input_origin) + fixture.truth.preamble_end_sample,
                "Observed upchirp lies outside the independently generated preamble");
    }
    std::cout << test.name << ": raw_subband_detections=" << found.size()
              << " consolidated_waveform_observations=" << result.consolidated.size()
              << " max_delimiter_error_input_samples=" << maximum_time_error
              << " input_s=" << static_cast<double>(fixture.samples.size()) / test.input_rate
              << " wall_s=" << wall << '\n';
    require(!found.empty(), "No waveform identification through the full PFB/discovery pipeline");
    require(result.consolidated.size() == 1, "Overlapping outputs did not consolidate one fixture waveform");
    const auto& observation = result.consolidated.front();
    require(observation.contributing_subbands.size() == found.size(), "Consolidation lost a contributing subband");
    require(std::abs(observation.delimiter_input_sample - expected_delimiter) < 1,
            "Consolidation lost fractional original-input timing");
    // This fixture contains one generated waveform. Consolidation does not
    // establish a unique on-air packet, transmitter or authenticated sender.
}

ovmesh::LoRaDiscovery observation_fixture() {
    ovmesh::LoRaDiscovery result;
    result.center_hz = receiver_center; result.bandwidth_hz = 250000; result.spreading_factor = 11;
    result.first_observed_upchirp_sample = 20.125; result.delimiter_sample = 1000.375;
    result.up_match = .6; result.down_match = .7; result.repeat_coherence = .8;
    return result;
}

void consolidation_checks() {
    ovmesh::DiscoveryObservations observations(16000000, 4);
    const auto first = observation_fixture();
    const auto a = observations.observe(first, 0, 100003, 8);
    auto stronger = first; stronger.center_hz += 10; stronger.delimiter_sample += .0625;
    stronger.up_match = .85; stronger.down_match = .9;
    const auto b = observations.observe(stronger, 1, 100003, 8);
    require(a.id == b.id && b.merged && !b.evicted && observations.observations().size() == 1,
            "Agreeing overlapping-subband observations must retain one stable ID");
    const auto& best = observations.observations().front();
    require(best.best_subband == 1 && best.received_center_hz == stronger.center_hz &&
            best.delimiter_input_sample == 100003 + stronger.delimiter_sample * 8 &&
            best.contributing_subbands == std::vector<std::size_t>{0, 1},
            "Consolidation must retain the best actual fit and contributing subbands");

    auto separate = first; separate.center_hz += 100000;
    const auto c = observations.observe(separate, 2, 100003, 8);
    require(!c.merged && c.id != a.id, "Distinct centers with the same BW/SF were consolidated");
    auto later = first; later.first_observed_upchirp_sample += 10000; later.delimiter_sample += 10000;
    const auto d = observations.observe(later, 0, 100003, 8);
    require(!d.merged && d.id != a.id, "Later same-profile transmission was consolidated with earlier activity");
    const auto e = observations.observe(later, 0, 100003, 8);
    require(!e.merged && e.id != d.id, "Same-subband repeated observations must not be hidden by overlap deduplication");
    auto nearby_later = first;
    nearby_later.first_observed_upchirp_sample += 32; nearby_later.delimiter_sample += 32;
    require(!observations.observe(nearby_later, 3, 100003, 8).merged,
            "Different-subband observation beyond two chips was merged with earlier activity");
    auto different_profile = first; different_profile.bandwidth_hz = 500000;
    require(!observations.observe(different_profile, 3, 100003, 8).merged,
            "Different bandwidth hypotheses were hidden by overlap consolidation");

    // Agreement must hold across the complete group, not drift via a chain of
    // individually nearby fits whose endpoints disagree by more than 2 bins.
    ovmesh::DiscoveryObservations drift(16000000, 3);
    drift.observe(first, 0, 100003, 8);
    auto middle = stronger; middle.center_hz = first.center_hz + 180;
    require(drift.observe(middle, 1, 100003, 8).merged, "Nearby fit did not join its overlap observation");
    auto distant = stronger; distant.center_hz = first.center_hz + 350;
    require(!drift.observe(distant, 2, 100003, 8).merged,
            "Pairwise frequency drift joined mutually incompatible observations");

    ovmesh::DiscoveryObservations ambiguous(16000000, 2);
    ambiguous.observe(first, 0, 100003, 8);
    ambiguous.observe(first, 0, 100003, 8);
    require(!ambiguous.observe(first, 1, 100003, 8).merged &&
            ambiguous.observations().back().association_ambiguous,
            "Ambiguous alternatives must remain separate rather than inventing identity");

    ovmesh::DiscoveryObservations bounded(8000000, 1);
    std::uint64_t last = 0;
    for (std::size_t i = 0; i < 260; ++i) {
        auto event = first;
        event.first_observed_upchirp_sample += static_cast<double>(i) * 10000;
        event.delimiter_sample += static_cast<double>(i) * 10000;
        const auto update = bounded.observe(event, 0, 99991, 4);
        require(update.id > last && update.evicted == (i >= ovmesh::DiscoveryObservations::capacity),
                "Bounded observations lost monotonic IDs or eviction status");
        last = update.id;
    }
    require(bounded.observations().size() == 256 && bounded.eviction_count() == 4 &&
            bounded.observations().front().id == 5,
            "Observation retention exceeds its bound or fails to expose eviction");
}

void malformed_observations() {
    for (const auto& invalid : {std::pair{16000000u, std::size_t{33}}, std::pair{3000000u, std::size_t{1}}}) {
        bool rejected = false;
        try { const ovmesh::DiscoveryObservations bad(invalid.first, invalid.second); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Invalid observation clock or contributor bound was accepted");
    }
    ovmesh::DiscoveryObservations observations(16000000, 2);
    const auto good = observation_fixture();
    auto rejects = [&](ovmesh::LoRaDiscovery value, std::size_t subband = 0,
                       std::uint64_t origin = 100003, std::uint32_t stride = 8) {
        const auto size = observations.observations().size(); bool rejected = false;
        try { observations.observe(value, subband, origin, stride); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && observations.observations().size() == size && observations.eviction_count() == 0,
                "Malformed observation changed retained results");
    };
    auto bad = good; bad.center_hz = std::numeric_limits<double>::quiet_NaN(); rejects(bad);
    bad = good; bad.down_match = std::numeric_limits<double>::infinity(); rejects(bad);
    bad = good; bad.up_match = -1; rejects(bad);
    bad = good; bad.bandwidth_hz = 123456; rejects(bad);
    bad = good; bad.spreading_factor = 13; rejects(bad);
    bad = good; bad.first_observed_upchirp_sample = -1; rejects(bad);
    bad = good; bad.delimiter_sample = bad.first_observed_upchirp_sample; rejects(bad);
    bad = good; bad.delimiter_sample = std::numeric_limits<double>::quiet_NaN(); rejects(bad);
    rejects(good, 2); rejects(good, 0, 100003, 7);
    rejects(good, 0, std::numeric_limits<std::uint64_t>::max());
    rejects(good, 0, (std::uint64_t{1} << 53) - 2);
    require(observations.observe(good, 0, 100003, 8).id == 1,
            "Rejected observations consumed an ID");
}
}

int main() {
    try {
        const Case cases[] = {
            {"center-crossing", 8000000, 250000, 9, 0, 31.25, -2000000, 2000000},
            {"lower-overlap-250", 8000000, 250000, 9, -500193.75, 73.125, -2000000, 2000000},
            {"upper-overlap-250", 8000000, 250000, 10, 499817.25, 31.75, -2000000, 2000000},
            {"lower-guard-500", 8000000, 500000, 10, -500137.5, 19.25, -2000000, 2000000},
            {"upper-guard-500", 8000000, 500000, 10, 500267.5, -83.25, -2000000, 2000000},
            {"requested-lower-edge", 16000000, 250000, 9, -1875000, 0, -2000000, 2000000},
            {"requested-upper-edge", 16000000, 500000, 9, 1750000, 0, -2000000, 2000000},
            {"wide-range-offgrid", 16000000, 500000, 8, 5749817.125, 12, -6000000, 6000000},
            {"longfast-truth-only", 8000000, 250000, 11, -625000, 0, -2000000, 2000000},
        };
        consolidation_checks(); malformed_observations();
        for (const auto& test : cases) check_case(test);
        std::cout << "Independent channelizer/discovery integration checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
