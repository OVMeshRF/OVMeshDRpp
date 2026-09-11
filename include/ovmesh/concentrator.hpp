// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ovmesh/survey.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ovmesh {
// One service modem per USB board. These are configured receive parameters,
// never an assertion that all bandwidths/SFs within a survey are decoded.
struct ConcentratorBoardConfig {
    std::string device_path;
    std::string device_id; // OS metadata identity; local preferences only.
    bool packets_enabled = true;
    uint64_t frequency_hz = 906875000;
    uint32_t bandwidth_hz = 250000;
    unsigned spreading_factor = 11;
    unsigned sync_word = 0x2b;
    bool operator==(const ConcentratorBoardConfig&) const = default;
};
struct ConcentratorConfig {
    std::vector<ConcentratorBoardConfig> boards{ConcentratorBoardConfig{}};
    bool scan_enabled = true;
    bool decode_enabled = true; // Authorized keyring only; packet RF metadata remains available.
    uint32_t scan_step_hz = 200000;
    unsigned scan_samples = 2000;
    bool operator==(const ConcentratorConfig&) const = default;
};

// SX1261 histogram measurements are sampled RF power, not continuous FFT
// occupancy. Host intervals bound each scan transaction, not exact RF dwell.
// Histogram order/threshold interpretation is documented with the HAL intake.
struct ConcentratorScan {
    uint64_t id = 0;
    unsigned board_index = 0;
    uint64_t frequency_hz = 0;
    uint32_t filter_bandwidth_hz = 234300;
    double utc_start_seconds = 0, utc_end_seconds = 0;
    double elapsed_start_seconds = 0, elapsed_end_seconds = 0;
    double rssi_offset_db = -11;
    std::array<uint32_t,33> counts{};
    std::optional<PositionFix> receiver_position;
};
struct ConcentratorPacketMetadata {
    unsigned board_index = 0;
    double rssi_dbm = 0; // Nominal vendor offset; uncalibrated.
    uint32_t hardware_timestamp_us = 0; // Board-local wrapping counter.
};
struct ConcentratorHealth {
    std::string state = "Idle";
    bool ready = false;
    uint64_t scans = 0, rssi_samples = 0, receptions = 0, crc_failures = 0;
    double ready_elapsed_seconds = 0, last_update_elapsed_seconds = 0;
};
// Pure validation, shared by engine/preferences/tests; never opens hardware.
void validate_concentrator_config(const ConcentratorConfig&, uint64_t center_hz,
                                 uint32_t span_hz, int64_t offset_hz,
                                 bool require_device_paths = true);
uint64_t concentrator_sample_count(const ConcentratorScan&);
// Fraction of samples in histogram bins wholly above this threshold. Quantized
// thresholds prevent an apparent precision finer than the 4 dB bins.
double concentrator_fraction_above(const ConcentratorScan&, double threshold_dbm);
double concentrator_bin_lower_dbm(unsigned bin, double rssi_offset_db = -11);
}
