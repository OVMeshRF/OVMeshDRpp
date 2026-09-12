// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace ovmesh {
struct PositionFix {
    double latitude = 0;
    double longitude = 0;
    std::optional<double> altitude_m;
    double utc_seconds = 0;
    double monotonic_seconds = 0;
    bool valid = false;
    bool manual = false;
    std::string source;
    std::optional<double> hdop;
    unsigned satellites = 0;
};

// Flags describe measurement limitations, never a transmitter identity.
enum SurveyQuality : uint32_t {
    SurveyUncalibrated = 1u << 0,
    SurveyUpstreamLossUnknown = 1u << 1,
    SurveyClipped = 1u << 2,
    SurveyPositionMissing = 1u << 3,
    SurveyBoundary = 1u << 4,
    SurveyTruncated = 1u << 5,
    SurveyMerged = 1u << 6,
    SurveyBackgroundUncertain = 1u << 7,
    SurveyDcSuspect = 1u << 8,
    SurveyPowerAggregated = 1u << 9
};

// Phase-free RF measurements only. Activity is a frame-major packed bitmap:
// ceil(bin_count/8) bytes per contiguous FFT, least significant bit first.
// Keeping the joint activity mask permits union occupancy over arbitrary widths.
struct SpectrumTile {
    uint64_t id = 0, first_sample = 0, end_sample = 0;
    double utc_start_seconds = 0, utc_end_seconds = 0;
    double elapsed_start_seconds = 0, elapsed_end_seconds = 0;
    // Power may describe a larger interval than the fine activity tile. Zero
    // bounds on newly measured tiles mean the tile's own interval.
    double power_elapsed_start = 0, power_elapsed_end = 0;
    double first_center_hz = 0, bin_width_hz = 0;
    uint32_t fft_size = 4096, frame_count = 0;
    float background_dbfs = -180;
    uint64_t clipped_samples = 0;
    uint32_t quality = SurveyUncalibrated;
    std::vector<float> mean_dbfs, peak_dbfs;
    std::vector<uint8_t> activity;
    std::optional<PositionFix> receiver_start, receiver_end;
};

// A connected above-threshold energy event. Overlap may merge events; chirps
// may fragment. Width is the observed frequency envelope, not a modem setting.
struct SpectrumEvent {
    uint64_t id = 0, first_sample = 0, end_sample = 0;
    double utc_start_seconds = 0, utc_end_seconds = 0;
    double elapsed_start_seconds = 0, elapsed_end_seconds = 0;
    double lower_hz = 0, upper_hz = 0;
    double active_seconds = 0;
    float mean_dbfs = -180, peak_dbfs = -180;
    uint32_t quality = SurveyUncalibrated;
    std::optional<PositionFix> receiver_start, receiver_end;
};

// Derived candidate burst, never a packet or an inferred modem bandwidth.
// Envelope duration includes short grouping gaps; active time does not.
struct SpectrumBurst {
    uint64_t id = 0, components = 0;
    double elapsed_start_seconds = 0, elapsed_end_seconds = 0;
    double lower_hz = 0, upper_hz = 0, active_seconds = 0;
    float peak_dbfs = -180;
    uint32_t quality = 0;
    bool center_region = false, ambiguous = false, limited = false;
};

// Waveform evidence from coherent preamble/reset/delimiter matching. These are
// inferred modem settings, not an energy envelope, packet decode or mesh identity.
// The time span covers observed preamble evidence only, never packet airtime.
struct WaveformObservation {
    uint64_t id = 0;
    double center_hz = 0;
    uint32_t bandwidth_hz = 0;
    unsigned spreading_factor = 0;
    double first_observed_elapsed = 0, delimiter_elapsed = 0, delimiter_utc = 0;
    double up_match = 0, down_match = 0;
    unsigned contributing_subbands = 0;
    bool complete_in_requested_range = false, association_ambiguous = false;
    std::optional<PositionFix> receiver_position;
};

struct DiscoveryBandCoverage {
    unsigned subband_index = 0;
    double center_hz = 0;
    uint64_t processed_samples = 0, abandoned_samples = 0;
    uint64_t source_gap_input_samples = 0;
    uint64_t candidate_limit_hits = 0, track_limit_hits = 0;
    // Live acquisition diagnostics only; not part of the saved survey schema.
    // Historical recordings leave availability false, rather than imply zero work.
    bool runtime_diagnostics_available = false;
    uint64_t fft_searches = 0, windows = 0, resets_after_gap = 0;
};

// Discovery coverage is separate from spectrum measurement coverage. Accepted
// samples are not necessarily processed; preamble history is lost at each gap.
struct DiscoveryStatus {
    bool enabled = false, finished = false, failed = false;
    std::string method = "lora-preamble-v2";
    std::string fault;
    uint64_t accepted_input_samples = 0, rejected_input_samples = 0;
    uint64_t channelized_input_samples = 0, abandoned_input_samples = 0;
    uint64_t source_queue_drops = 0, stream_resets = 0;
    uint64_t result_overflows = 0, gap_overflows = 0;
    uint64_t observations = 0;
    std::vector<DiscoveryBandCoverage> bands;
};

struct DiscoveryGap {
    uint64_t id = 0, first_input_sample = 0, end_input_sample = 0;
    // -1 covers the entire requested range; otherwise the processing subband.
    int subband_index = -1;
    std::string reason;
};

struct CoverageGap {
    uint64_t id = 0, missing_samples = 0;
    double utc_start_seconds = 0, utc_end_seconds = 0;
    double elapsed_start_seconds = 0, elapsed_end_seconds = 0;
    std::string reason;
};

struct SurveyQuery {
    // Zero frequency bounds mean the recorded usable range. Time is elapsed
    // seconds from session start. A zero end means the complete session.
    double lower_hz = 0, upper_hz = 0, elapsed_start = 0, elapsed_end = 0;
    bool geographic_filter = false;
    double south = -90, north = 90, west = -180, east = 180;
    // Requested minimum display width. Analysis may coarsen it to cover the
    // entire selected interval within max_observations (zero disables points).
    double time_bucket_seconds = 1;
    size_t max_observations = 2000, max_events = 200;
};
struct SurveyObservation {
    double elapsed_start = 0, elapsed_end = 0;
    double observed_seconds = 0, busy_seconds = 0;
    double outside_center_busy_seconds = 0, center_busy_seconds = 0;
    double outside_center_observed_seconds = 0;
    double mean_dbfs = -180, peak_dbfs = -180, background_dbfs = -180;
    uint32_t quality = 0;
    std::optional<PositionFix> receiver_position;
};
struct SurveyBin {
    double center_hz = 0, width_hz = 0;
    double observed_seconds = 0, active_seconds = 0;
    double mean_dbfs = -180, peak_dbfs = -180;
};
struct SurveyAnalysis {
    // Multiple acquisition settings may have distinct grids and center guards.
    bool mixed_acquisitions = false;
    bool detailed_available = false, observations_truncated = false, events_truncated = false;
    bool observations_coarsened = false;
    uint64_t tile_count = 0, event_count = 0;
    double covered_lower_hz = 0, covered_upper_hz = 0, bin_width_hz = 0;
    // Resolved time selection includes recorded gaps. Display buckets cover this
    // entire interval, growing beyond the requested width when needed to fit.
    double resolved_elapsed_start = 0, resolved_elapsed_end = 0;
    double effective_time_bucket_seconds = 0;
    double observed_seconds = 0, busy_seconds = 0;
    // Observed time whose stored tile endpoints lack a receiver position.
    // Start/end are independent; neither duration is an acquisition gap.
    double missing_start_position_seconds = 0, missing_end_position_seconds = 0;
    // A diagnostic only: bins whose centers lie within two bin widths of the
    // nominal receiver center. Raw busy time, bins, and saved data stay intact.
    double center_guard_lower_hz = 0, center_guard_upper_hz = 0;
    size_t center_guard_bin_count = 0, outside_center_bin_count = 0;
    double outside_center_busy_seconds = 0, center_busy_seconds = 0;
    double outside_center_observed_seconds = 0;
    uint32_t quality = 0;
    std::vector<SurveyBin> bins;
    std::vector<SurveyObservation> observations;
    std::vector<SpectrumEvent> events;
    uint64_t burst_count = 0;
    std::vector<SpectrumBurst> bursts;
    std::vector<CoverageGap> gaps;
    uint64_t waveform_count = 0;
    bool waveforms_truncated = false;
    std::vector<WaveformObservation> waveforms;
};
}
