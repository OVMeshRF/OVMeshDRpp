// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "lora_discovery.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ovmesh {

// Experimental waveform observation. Agreement across overlapping PFB outputs
// is not a unique-packet, sender, transmitter or mesh-protocol identity.
struct DiscoveryObservation {
    std::uint64_t id = 0;
    double received_center_hz = 0;
    std::uint32_t bandwidth_hz = 0;
    unsigned spreading_factor = 0;
    double first_observed_upchirp_input_sample = 0, delimiter_input_sample = 0;
    double up_match = 0, down_match = 0, repeat_coherence = 0;
    std::size_t best_subband = 0;
    std::vector<std::size_t> contributing_subbands;
    // Bounds prevent successive pairwise matches from drifting into a much
    // wider frequency/time association. They include every contributing fit.
    double center_min_hz = 0, center_max_hz = 0;
    double delimiter_min_input_sample = 0, delimiter_max_input_sample = 0;
    bool association_ambiguous = false;
};

class DiscoveryObservations {
public:
    static constexpr std::size_t capacity = 256, maximum_subbands = 32;
    struct Update { std::uint64_t id; bool merged, evicted; };

    explicit DiscoveryObservations(std::uint32_t input_sample_rate, std::size_t subband_count)
        : rate_(input_sample_rate), subbands_(subband_count) {
        if (rate_ < 2000000 || rate_ > 20000000 || rate_ % 2000000 ||
            !subbands_ || subbands_ > maximum_subbands)
            throw std::invalid_argument("Invalid discovery observation clock or subband count");
        recent_.reserve(capacity);
    }

    Update observe(const LoRaDiscovery& found, std::size_t subband,
                   std::uint64_t first_input_sample, std::uint32_t stride) {
        // Mapped coordinates must retain meaningful original-sample precision.
        constexpr std::uint64_t maximum_coordinate = std::uint64_t{1} << 53;
        const auto score_valid = [](double value) {
            return std::isfinite(value) && value >= 0 && value <= 1.00001;
        };
        if (subband >= subbands_ || stride != rate_ / 2000000 ||
            first_input_sample > maximum_coordinate ||
            !std::isfinite(found.center_hz) || found.center_hz <= 0 ||
            (found.bandwidth_hz != 15625 && found.bandwidth_hz != 62500 && found.bandwidth_hz != 125000 && found.bandwidth_hz != 250000 && found.bandwidth_hz != 500000) ||
            found.spreading_factor < 7 || found.spreading_factor > 12 ||
            found.center_hz < found.bandwidth_hz / 2. ||
            !std::isfinite(found.first_observed_upchirp_sample) ||
            !std::isfinite(found.delimiter_sample) || found.first_observed_upchirp_sample < 0 ||
            found.delimiter_sample <= found.first_observed_upchirp_sample ||
            !score_valid(found.up_match) || !score_valid(found.down_match) ||
            !score_valid(found.repeat_coherence) || found.up_match <= 0 || found.down_match <= 0)
            throw std::invalid_argument("Invalid discovery waveform observation");
        const long double maximum_local = static_cast<long double>(maximum_coordinate - first_input_sample) / stride;
        if (static_cast<long double>(found.delimiter_sample) > maximum_local)
            throw std::invalid_argument("Discovery observation sample coordinate exceeds precise range");

        DiscoveryObservation candidate;
        candidate.received_center_hz = found.center_hz;
        candidate.bandwidth_hz = found.bandwidth_hz; candidate.spreading_factor = found.spreading_factor;
        candidate.first_observed_upchirp_input_sample = static_cast<double>(first_input_sample) +
            found.first_observed_upchirp_sample * stride;
        candidate.delimiter_input_sample = static_cast<double>(first_input_sample) + found.delimiter_sample * stride;
        candidate.up_match = found.up_match; candidate.down_match = found.down_match;
        candidate.repeat_coherence = found.repeat_coherence; candidate.best_subband = subband;
        candidate.contributing_subbands.push_back(subband);
        candidate.center_min_hz = candidate.center_max_hz = candidate.received_center_hz;
        candidate.delimiter_min_input_sample = candidate.delimiter_max_input_sample = candidate.delimiter_input_sample;

        const double frequency_tolerance = 2. * found.bandwidth_hz / (std::uint32_t{1} << found.spreading_factor);
        const double time_tolerance = 2. * rate_ / found.bandwidth_hz;
        std::size_t match = recent_.size(), matches = 0;
        for (std::size_t i = 0; i < recent_.size(); ++i) {
            auto& previous = recent_[i];
            if (previous.bandwidth_hz != candidate.bandwidth_hz ||
                previous.spreading_factor != candidate.spreading_factor ||
                std::find(previous.contributing_subbands.begin(), previous.contributing_subbands.end(), subband) !=
                    previous.contributing_subbands.end()) continue;
            if (std::max(previous.center_max_hz, candidate.received_center_hz) -
                    std::min(previous.center_min_hz, candidate.received_center_hz) > frequency_tolerance ||
                std::max(previous.delimiter_max_input_sample, candidate.delimiter_input_sample) -
                    std::min(previous.delimiter_min_input_sample, candidate.delimiter_input_sample) > time_tolerance)
                continue;
            match = i; ++matches;
        }
        if (matches == 1) {
            auto& previous = recent_[match];
            const auto id = previous.id;
            const double low = std::min(previous.center_min_hz, candidate.received_center_hz);
            const double high = std::max(previous.center_max_hz, candidate.received_center_hz);
            const double early = std::min(previous.delimiter_min_input_sample, candidate.delimiter_input_sample);
            const double late = std::max(previous.delimiter_max_input_sample, candidate.delimiter_input_sample);
            auto contributors = previous.contributing_subbands;
            contributors.push_back(subband); std::sort(contributors.begin(), contributors.end());
            const bool ambiguous = previous.association_ambiguous;
            // Preserve one measured fit, not an averaged fictional waveform.
            // Best fit is the larger minimum of the up/down match scores.
            if (std::min(candidate.up_match, candidate.down_match) > std::min(previous.up_match, previous.down_match))
                previous = std::move(candidate);
            previous.id = id; previous.contributing_subbands = std::move(contributors);
            previous.center_min_hz = low; previous.center_max_hz = high;
            previous.delimiter_min_input_sample = early; previous.delimiter_max_input_sample = late;
            previous.association_ambiguous = ambiguous;
            return {id, true, false};
        }
        candidate.association_ambiguous = matches > 1;
        if (next_id_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Discovery observation ID exhausted");
        const bool evicted = recent_.size() == capacity;
        if (evicted) { recent_.erase(recent_.begin()); ++evictions_; }
        candidate.id = next_id_++;
        const auto id = candidate.id; recent_.push_back(std::move(candidate));
        return {id, false, evicted};
    }

    // Ordered by first insertion, not packet time. Merging preserves the ID;
    // FIFO eviction is a bounded recent view, not a durable observation count.
    const std::vector<DiscoveryObservation>& observations() const noexcept { return recent_; }
    std::uint64_t eviction_count() const noexcept { return evictions_; }

private:
    std::uint32_t rate_;
    std::size_t subbands_;
    std::uint64_t next_id_ = 1, evictions_ = 0;
    std::vector<DiscoveryObservation> recent_;
};
}
