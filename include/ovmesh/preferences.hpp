// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ovmesh/concentrator.hpp"

#include <cstdint>
#include <string>

namespace ovmesh {

// Desktop convenience settings only. Never add channel keys, decoded content,
// coordinates, or an instruction to start RF reception to this file.
enum class DesktopReceiver : uint8_t { Synthetic = 0, HackRf = 1, RtlSdr = 2, Rak5146 = 3 };

struct DesktopPreferences {
    bool gps_enabled = true;
    bool recording_enabled = true;
    bool compact_recording = true;
    bool discover_lora = true;
    bool decode_enabled = true;
    bool spectrum_only = false;
    // A preferred source is not a command to open it. The desktop must handle
    // unavailable backends and keep reception stopped on ordinary startup.
    DesktopReceiver receiver_source = DesktopReceiver::HackRf;
    uint64_t center_hz = 907500000;
    uint32_t sample_rate = 16000000;
    uint32_t survey_span_hz = 10000000;
    unsigned lna_gain = 16;
    unsigned vga_gain = 16;
    bool amplifier = false;
    int rtl_gain_tenths_db = 280;
    bool rtl_auto_gain = false;
    ConcentratorConfig concentrators;
    bool mixed_fonts = true;
    bool mobile_position_display = false;
    std::string recording_directory;
    std::string gps_device_id;
    unsigned gps_baud = 9600;
    int64_t tuning_offset_hz = 0;
    bool operator==(const DesktopPreferences&) const = default;
};

struct PreferencePaths {
    std::string directory;
    std::string settings_file;
    std::string surveys_directory;
};

// All paths are UTF-8. The explicit override supports isolated local tests.
PreferencePaths preference_paths(const std::string& explicit_directory = {});
// Creates only the owned preference and default survey directories. Existing
// profile ancestors are left unchanged; owned directories must be private.
void ensure_preferences_directories(const PreferencePaths& paths);
// Loading is read-only. Missing settings return defaults; malformed or unsafe
// settings throw and are never overwritten by save_preferences.
DesktopPreferences load_preferences(const PreferencePaths& paths);
void save_preferences(const PreferencePaths& paths, const DesktopPreferences& preferences);
// Returns a new random, timestamped filename in an existing local directory.
// It creates nothing; session storage remains responsible for exclusive create.
std::string new_survey_path(const std::string& directory);

} // namespace ovmesh
