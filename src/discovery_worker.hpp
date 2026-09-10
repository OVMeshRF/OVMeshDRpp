// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "discovery_channelizer.hpp"
#include "lora_discovery.hpp"
#include <complex>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ovmesh {

// Experimental background discovery only. No receiver, filesystem, transport,
// decoder-key or UI dependency. This wrapper does not establish live capacity.
class DiscoveryWorker {
public:
    static constexpr size_t maximum_input_block = 131072;
    // 64 MiB of bounded transient source IQ (0.524288 seconds at 16 MS/s).
    // This absorbs short discovery bursts; it cannot fix sustained overload.
    static constexpr size_t source_capacity = 64;
    static constexpr size_t detector_workers = 3;
    static constexpr size_t detector_queue_capacity = 16;
    static constexpr size_t detector_batch = 4096;
    static constexpr size_t result_capacity = 128, gap_capacity = 128;
    static constexpr size_t all_subbands = std::numeric_limits<size_t>::max();

    enum class GapReason {
        source_queue_full, input_discontinuity,
        input_too_large, invalid_input_order, invalid_sample_coordinate,
        processing_failure
    };
    struct Gap {
        uint64_t first_input_sample = 0, end_input_sample = 0;
        size_t subband_index = all_subbands;
        GapReason reason = GapReason::input_discontinuity;
    };
    struct Result {
        size_t subband_index = 0;
        uint64_t segment_id = 0, first_input_anchor = 0;
        uint32_t input_sample_stride = 0;
        LoRaDiscovery waveform;
        double first_observed_upchirp_input_sample = 0, delimiter_input_sample = 0;
        bool complete_in_requested_range = false;
    };
    struct SubbandProgress {
        DiscoveryChannelizer::Subband band;
        uint64_t processed_output_samples = 0;
        uint64_t first_processed_input_sample = 0;
        uint64_t last_processed_input_sample = 0; // exclusive sample-center interval
        uint64_t source_gap_input_samples = 0;
        uint64_t abandoned_output_samples = 0;
        uint64_t resets_after_gap = 0;
        uint64_t discoveries = 0, result_overflows = 0;
        uint64_t outside_range_candidates = 0;
        uint64_t candidate_limit_hits = 0, track_limit_hits = 0;
        uint64_t fft_searches = 0, windows = 0;
    };
    struct Snapshot {
        bool input_closed = false, finished = false, failed = false;
        uint64_t accepted_input_samples = 0, rejected_input_samples = 0;
        uint64_t channelized_input_samples = 0, abandoned_input_samples = 0;
        uint64_t source_gap_input_samples = 0;
        uint64_t source_queue_drops = 0;
        uint64_t invalid_submissions = 0, rejected_after_close = 0;
        uint64_t stream_resets = 0, result_overflows = 0, gap_overflows = 0;
        size_t source_queue_high_water = 0;
        std::vector<size_t> detector_queue_high_water;
        size_t queued_results = 0, queued_gaps = 0;
        // Fixed application wording, at most 160 characters. Never exception
        // dumps, paths, payloads or arbitrary caller text.
        std::string fault;
        std::vector<SubbandProgress> subbands;
    };

    DiscoveryWorker(uint32_t input_sample_rate, double receiver_center_hz,
                    double requested_lower_hz, double requested_upper_hz);
    ~DiscoveryWorker();
    DiscoveryWorker(const DiscoveryWorker&) = delete;
    DiscoveryWorker& operator=(const DiscoveryWorker&) = delete;

    // One caller owns submit(). Copies at most maximum_input_block samples into
    // a preallocated slot. Never waits for DSP or queue space. Rejected intervals
    // are reported and the next accepted segment resets the discovery state.
    // A brief metadata mutex may be acquired; no DSP executes under that mutex.
    // Original input coordinates must be increasing and at most 2^53 so mapped
    // floating-point result times retain original-sample precision.
    bool submit(std::span<const std::complex<float>> input, uint64_t first_input_sample);
    // Close input, drain accepted work and join all workers. May wait for bounded
    // DSP work. Idempotent; never call from a worker callback (none are exposed).
    void finish();
    [[nodiscard]] Snapshot snapshot() const;
    std::vector<Result> take_results();
    std::vector<Gap> take_gaps();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ovmesh
