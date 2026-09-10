// SPDX-License-Identifier: GPL-3.0-or-later
// Offline bounded-worker tests. No USB, files, RF transmission or ordinary app.
#include "discovery_worker.hpp"
#include "lora_discovery_fixtures.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Worker = ovmesh::DiscoveryWorker;
using Complex = std::complex<float>;
namespace f = lora_discovery_fixtures;
constexpr double center = 907500000;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

void wait_channelized(Worker& worker, uint64_t expected) {
    // A full 64-slot source queue includes over half a second of wideband DSP.
    // Thread instrumentation can take much longer than the optimized receiver;
    // this is a correctness deadline, not a live-capacity assertion.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (worker.snapshot().channelized_input_samples < expected) {
        require(!worker.snapshot().failed, "Background processing failed while waiting");
        require(std::chrono::steady_clock::now() < deadline, "Bounded worker failed to make progress");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Keep this a paced correctness fixture, not a source-capacity test. The
    // separate saturation test submits at full speed and explicitly accepts loss.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

std::vector<Worker::Result> direct(const std::vector<Complex>& input, uint64_t origin) {
    ovmesh::DiscoveryChannelizer bank(8000000, center, center - 1000000, center + 1000000);
    std::vector<std::unique_ptr<ovmesh::LoRaPreambleDiscovery>> detectors;
    std::vector<uint64_t> next(bank.subbands().size()), anchors(bank.subbands().size());
    for (const auto& band : bank.subbands())
        detectors.push_back(std::make_unique<ovmesh::LoRaPreambleDiscovery>(band.center_hz));
    std::vector<Worker::Result> results;
    bank.feed(input, origin, [&](const auto& samples) {
        const size_t k = samples.subband_index;
        if (!next[k]) anchors[k] = samples.first_input_sample;
        detectors[k]->feed(samples.values, next[k], [&](const auto& found) {
            Worker::Result result;
            result.subband_index = k;
            result.waveform = found;
            result.first_input_anchor = anchors[k];
            result.input_sample_stride = samples.input_sample_stride;
            result.first_observed_upchirp_input_sample = static_cast<double>(anchors[k]) +
                found.first_observed_upchirp_sample * samples.input_sample_stride;
            result.delimiter_input_sample = static_cast<double>(anchors[k]) +
                found.delimiter_sample * samples.input_sample_stride;
            results.push_back(result);
        });
        next[k] += samples.values.size();
    });
    return results;
}
void sort_results(std::vector<Worker::Result>& results) {
    std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) {
        if (a.subband_index != b.subband_index) return a.subband_index < b.subband_index;
        return a.delimiter_input_sample < b.delimiter_input_sample;
    });
}

void waveform_and_timing() {
    f::PreambleSpec spec;
    spec.sample_rate_hz = 8000000;
    spec.bandwidth_hz = 250000;
    spec.spreading_factor = 8;
    spec.center_offset_hz = 123719;
    spec.cfo_hz = 917.25;
    spec.leading_samples = 751;
    spec.fractional_start_samples = .375;
    spec.noise_rms = .01;
    const auto fixture = f::make_preamble(spec);
    constexpr uint64_t origin = 53;
    auto reference = direct(fixture.samples, origin);
    require(!reference.empty(), "Independent waveform has no direct discovery baseline");
    sort_results(reference);
    for (size_t chunk : {Worker::maximum_input_block, size_t{15011}}) {
        Worker worker(8000000, center, center - 1000000, center + 1000000);
        size_t accepted = 0;
        for (size_t i = 0; i < fixture.samples.size(); i += chunk) {
            const auto count = std::min(chunk, fixture.samples.size() - i);
            require(worker.submit(std::span(fixture.samples).subspan(i, count), origin + i),
                    "Paced waveform input was rejected");
            accepted += count;
            wait_channelized(worker, accepted);
        }
        worker.finish();
        worker.finish();
        const auto state = worker.snapshot();
        require(state.finished && state.input_closed && !state.failed, "Final drain failed");
        require(state.accepted_input_samples == fixture.samples.size() &&
                state.channelized_input_samples == fixture.samples.size() &&
                state.rejected_input_samples == 0 && state.source_gap_input_samples == 0,
                "Continuous input lost coverage");
        auto actual = worker.take_results();
        sort_results(actual);
        require(actual.size() == reference.size(), "Async wrapper changed discovery count");
        for (size_t i = 0; i < actual.size(); ++i) {
            const auto& a = actual[i]; const auto& b = reference[i];
            require(a.subband_index == b.subband_index && a.waveform.center_hz == b.waveform.center_hz &&
                    a.waveform.bandwidth_hz == b.waveform.bandwidth_hz &&
                    a.waveform.spreading_factor == b.waveform.spreading_factor &&
                    a.first_observed_upchirp_input_sample == b.first_observed_upchirp_input_sample &&
                    a.delimiter_input_sample == b.delimiter_input_sample,
                    "Async wrapper changed a waveform or fractional timestamp");
            require(a.first_input_anchor == b.first_input_anchor && a.input_sample_stride == 4 &&
                    a.segment_id == 1 && a.complete_in_requested_range,
                    "Async wrapper lost its source-clock mapping");
        }
        require(worker.take_results().empty(), "Result drain duplicates observations");
        require(worker.take_gaps().empty(), "Continuous source reported a gap");
        require(!worker.submit(std::span(fixture.samples).first(1), origin + fixture.samples.size()),
                "Closed input accepted work");
    }
}

void gaps_order_and_limits() {
    std::vector<Complex> input(4096);
    Worker worker(8000000, center, center - 250000, center + 250000);
    require(worker.submit(input, 0), "Initial gap-test block rejected");
    wait_channelized(worker, input.size());
    require(!worker.submit(input, 2048), "Backward/overlapping input was accepted");
    require(worker.submit(input, 10000), "Post-gap block rejected");
    worker.finish();
    const auto state = worker.snapshot();
    require(!state.failed && state.stream_resets == 1 && state.invalid_submissions == 1,
            "Input gap or invalid order was not accounted for");
    require(state.source_gap_input_samples == 10000 - input.size(),
            "Invalid overlap inflated elapsed source-gap duration");
    require(state.subbands.size() == 1 && state.subbands[0].resets_after_gap == 1 &&
            state.subbands[0].source_gap_input_samples == state.source_gap_input_samples &&
            state.subbands[0].processed_output_samples > 0,
            "Per-subband reset/coverage counters are wrong");
    const auto gaps = worker.take_gaps();
    require(gaps.size() == 2 && worker.take_gaps().empty(), "Gap records were lost or duplicated");
    require(gaps[1].first_input_sample == 4096 && gaps[1].end_input_sample == 10000 &&
            gaps[1].reason == Worker::GapReason::input_discontinuity,
            "Source gap endpoints changed");

    Worker oversized(8000000, center, center - 250000, center + 250000);
    std::vector<Complex> large(Worker::maximum_input_block + 1);
    require(!oversized.submit(large, 0), "Oversized source block was accepted");
    require(!oversized.submit(input, (uint64_t{1} << 53) - 2048), "Imprecise sample coordinates accepted");
    require(oversized.submit(input, large.size()), "Valid input after a rejected interval failed");
    oversized.finish();
    const auto limit = oversized.snapshot();
    require(limit.source_gap_input_samples == large.size() && limit.invalid_submissions == 2 &&
            limit.channelized_input_samples == input.size(), "Input limit accounting failed");
}

void saturation() {
    Worker worker(16000000, center, center - 6000000, center + 6000000);
    std::vector<Complex> input(Worker::maximum_input_block);
    uint64_t accepted = 0, rejected = 0;
    constexpr size_t source_blocks = Worker::source_capacity * 4;
    for (size_t i = 0; i < source_blocks; ++i) {
        if (worker.submit(input, i * input.size())) accepted += input.size();
        else rejected += input.size();
    }
    wait_channelized(worker, accepted);
    require(worker.submit(input, source_blocks * input.size()), "Fresh input after saturation was rejected");
    accepted += input.size();
    worker.finish();
    const auto state = worker.snapshot();
    require(accepted && rejected && !state.failed, "Saturation fixture did not exercise queue rejection");
    require(state.accepted_input_samples == accepted && state.rejected_input_samples == rejected &&
            state.channelized_input_samples == accepted && state.source_gap_input_samples == rejected,
            "Rejected or processed source samples were silently lost");
    require(state.source_queue_high_water <= Worker::source_capacity &&
            state.source_queue_drops > 0,
            "Source queue was not bounded/accounted");
    for (size_t value : state.detector_queue_high_water)
        require(value <= Worker::detector_queue_capacity, "Detector queue exceeded its bound");
    for (const auto& band : state.subbands)
        require(band.source_gap_input_samples == rejected && band.resets_after_gap > 0,
                "A subband hides rejected source coverage or bridges a dropped interval");
    require(state.stream_resets > 0, "Post-saturation input did not reset channelizer state");
    require(!worker.take_gaps().empty(), "Source saturation produced no interval evidence");

    Worker gaps(8000000, center, center - 250000, center + 250000);
    for (size_t i = 0; i < 150; ++i) (void)gaps.submit(std::span(input).first(32), i * 64);
    gaps.finish();
    require(gaps.snapshot().queued_gaps <= Worker::gap_capacity && gaps.snapshot().gap_overflows > 0,
            "Gap metadata queue overflow was hidden or unbounded");
}

void failures_and_shutdown() {
    bool rejected = false;
    try { Worker invalid(123, center, center - 250000, center + 250000); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Invalid construction started workers");

    Worker failed(8000000, center, center - 250000, center + 250000);
    std::vector<Complex> input(4096);
    require(failed.submit(input, 0), "Pre-failure source interval was not submitted");
    wait_channelized(failed, input.size());
    input[0] = {std::numeric_limits<float>::quiet_NaN(), 0};
    require(failed.submit(input, input.size()), "Fault fixture was not submitted");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!failed.snapshot().failed) {
        require(std::chrono::steady_clock::now() < deadline, "Fault fixture did not fail");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    input[0] = {};
    require(!failed.submit(input, 2 * input.size()) &&
            !failed.submit(input, 3 * input.size()), "Failed worker accepted later RF input");
    require(!failed.submit(input, 3 * input.size()), "Failed worker accepted duplicate RF input");
    require(!failed.submit(input, 5 * input.size()), "Failed worker accepted RF input after a gap");
    failed.finish();
    const auto state = failed.snapshot();
    require(state.finished && state.failed && !state.fault.empty() && state.fault.size() <= 160 &&
            state.abandoned_input_samples == input.size(), "Background exception was not bounded/accounted");
    require(state.rejected_input_samples == 4 * input.size() &&
            state.source_gap_input_samples == 4 * input.size() &&
            state.invalid_submissions == 1 && state.rejected_after_close == 0,
            "Continued RF after failure lost coverage or counted duplicate time");
    const auto gaps = failed.take_gaps();
    require(!gaps.empty() && state.subbands[0].abandoned_output_samples > 0 &&
            std::any_of(gaps.begin(), gaps.end(), [&](const auto& gap) {
                return gap.subband_index == 0 && gap.first_input_sample < input.size();
            }), "Failure hides a staged pre-failure subband interval");
    uint64_t later_failure_samples = 0;
    for (const auto& gap : gaps)
        if (gap.reason == Worker::GapReason::processing_failure &&
            gap.subband_index == Worker::all_subbands && gap.end_input_sample > 2 * input.size())
            later_failure_samples += gap.end_input_sample - std::max<uint64_t>(gap.first_input_sample, 2 * input.size());
    require(later_failure_samples == 3 * input.size(),
            "Later RF after failure has no processing-failure interval evidence");
    require(!failed.submit(input, 6 * input.size()) && failed.snapshot().rejected_after_close == 1 &&
            failed.snapshot().source_gap_input_samples == state.source_gap_input_samples,
            "Explicitly closed input extended RF coverage");
    // Destructor must safely drain/join with accepted source work pending.
    { Worker pending(8000000, center, center - 250000, center + 250000);
      require(pending.submit(input, 0), "Destructor fixture was not submitted"); }
}

void result_queue_bound() {
    f::PreambleSpec spec;
    spec.sample_rate_hz = 8000000;
    spec.bandwidth_hz = 500000;
    spec.spreading_factor = 7;
    spec.noise_rms = .001;
    const auto fixture = f::make_preamble(spec);
    Worker worker(8000000, center, center - 250000, center + 250000);
    uint64_t count = 0;
    for (size_t i = 0; i < Worker::result_capacity + 8; ++i) {
        require(worker.submit(fixture.samples, count), "Paced result-overflow input rejected");
        count += fixture.samples.size();
        wait_channelized(worker, count);
    }
    worker.finish();
    const auto state = worker.snapshot();
    require(!state.failed && state.queued_results == Worker::result_capacity && state.result_overflows > 0,
            "Result overflow is unbounded or not reported");
    require(state.subbands[0].discoveries == state.queued_results + state.result_overflows &&
            state.subbands[0].result_overflows == state.result_overflows,
            "Per-subband result loss was hidden");
}

void requested_range_edges() {
    for (bool intersects : {false, true}) {
        f::PreambleSpec spec;
        spec.sample_rate_hz = 8000000;
        spec.bandwidth_hz = intersects ? 500000 : 125000;
        spec.spreading_factor = 7;
        spec.center_offset_hz = intersects ? 450000 : 500000;
        spec.noise_rms = .001;
        const auto fixture = f::make_preamble(spec);
        Worker worker(8000000, center, center - 250000, center + 250000);
        require(worker.submit(fixture.samples, 0), "Range-edge fixture was not submitted");
        worker.finish();
        const auto state = worker.snapshot();
        const auto results = worker.take_results();
        require(!state.failed, "Range-edge fixture failed");
        if (intersects) {
            require(!results.empty(), "Boundary-intersecting waveform was discarded");
            for (const auto& result : results)
                require(!result.complete_in_requested_range && result.waveform.center_hz > center + 250000,
                        "Boundary waveform claims full requested-range coverage");
        } else {
            require(results.empty() && state.subbands[0].discoveries == 0 &&
                    state.subbands[0].outside_range_candidates > 0,
                    "Guard-only waveform inflated requested-range discovery counts");
        }
    }
}
} // namespace

int main() {
    try {
        waveform_and_timing();
        gaps_order_and_limits();
        saturation();
        failures_and_shutdown();
        result_queue_bound();
        requested_range_edges();
        std::cout << "Experimental discovery worker ordering, clock, saturation, reset, drain and exception tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
