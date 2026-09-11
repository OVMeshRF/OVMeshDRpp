// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ovmesh/protocol.hpp"
#include "ovmesh/phy.hpp"
#include "ovmesh/survey.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ovmesh {

struct ReportOptions;

enum class HardwareReceiver { HackRf, RtlSdr };

struct LaneConfig {
    std::string label = "LongFast";
    std::string channel_name = "LongFast";
    std::string protocol = "Meshtastic";
    uint64_t frequency_hz = 906875000;
    uint32_t bandwidth_hz = 250000;
    uint8_t spreading_factor = 11;
    uint8_t coding_rate = 5;
    bool enabled = true;
};

struct ReceiverConfig {
    bool synthetic = true;
    HardwareReceiver hardware_receiver = HardwareReceiver::HackRf;
    uint64_t center_hz = 907500000;
    // Positive values raise the hardware tune command, matching SDR++ manual offset.
    // Spectrum/profile frequencies remain on the nominal, user-calibrated axis.
    int64_t tuning_offset_hz = 0;
    uint32_t sample_rate = 16000000;
    uint32_t survey_span_hz = 10000000;
    unsigned lna_gain = 16;
    unsigned vga_gain = 16;
    bool amplifier = false;
    int rtl_gain_tenths_db = 280;
    bool rtl_auto_gain = false;
    std::string device_serial;
    float activity_threshold_dbfs = -55.0f;
    bool discover_lora = false;
    // New surveys retain fine activity timing but summarize routine power at
    // approximately one second. False preserves detailed 20 ms recording.
    bool compact_recording = true;
    std::vector<LaneConfig> lanes{LaneConfig{}};
    std::string session_path;
    std::string session_title = "RF survey";
    // Descriptive acquisition provenance; never used to infer calibration.
    std::string antenna_description;
    std::string receiver_description;
    std::string survey_notes;
};

// Validates the nominal center, bounded +/-100 kHz correction, and corrected tune.
// Does not access hardware. Synthetic RF models this offset as a receive-frequency shift.
uint64_t tuned_center_hz(const ReceiverConfig& config);
// Stable display/provenance label; never enumerates or opens a device.
const char* receiver_source_name(const ReceiverConfig& config) noexcept;

struct Reception {
    uint64_t id = 0;
    double utc_seconds = 0;
    double elapsed_seconds = 0;
    uint64_t frequency_hz = 0;
    uint32_t bandwidth_hz = 0;
    unsigned spreading_factor = 0;
    unsigned coding_rate = 0;
    double duration_seconds = 0;
    double snr_db = 0;
    double frequency_error_hz = 0;
    bool header_valid = false;
    bool crc_valid = false;
    std::string lane_label;
    protocol::DecodeResult decoded;
    std::optional<PositionFix> receiver_position;
};

struct FrequencySummary {
    uint64_t center_hz = 0;
    uint32_t width_hz = 0;
    double mean_dbfs = -120;
    double peak_dbfs = -120;
    double observed_seconds = 0;
    double active_seconds = 0;
};

struct LaneHealth {
    std::string label;
    uint64_t frequency_hz = 0;
    double processed_seconds = 0;
    uint64_t frames = 0;
    uint64_t decoded = 0;
    uint64_t crc_failures = 0;
    uint64_t resets = 0;
    std::string state = "idle";
    // Live aggregate acquisition diagnostics. No samples or undecoded bytes;
    // not yet persisted by the version-one survey schema.
    PhyDiagnostics phy;
};

enum class GpsConnectionState { Disconnected, WaitingForFix, ValidFix, StaleFix, ReadError };

// Live serial transport/fix state, independent of historical survey positions.
// Contains no NMEA sentences or coordinates and is not part of the saved schema.
struct GpsConnectionStatus {
    GpsConnectionState state = GpsConnectionState::Disconnected;
    std::string device_path;
    std::string detail = "GPS disconnected";
};

struct Snapshot {
    ReceiverConfig config;
    bool running = false;
    bool recording = false;
    bool historical = false;
    bool incomplete = false;
    bool hardware_available = false; // Build capability only; never USB enumeration.
    bool rtl_sdr_available = false; // Build capability only; never USB enumeration.
    std::string state = "Idle";
    std::string error;
    std::string session_id;
    std::string gps_status = "GPS disconnected";
    double elapsed_seconds = 0;
    double input_seconds = 0;
    double measurement_seconds = 0;
    double processing_load = 0;
    uint64_t delivered_samples = 0;
    uint64_t dropped_samples = 0;
    bool upstream_loss_unknown = true;
    uint64_t total_receptions = 0;
    uint64_t authorized_messages = 0;
    uint64_t spectrum_sequence = 0;
    std::vector<float> spectrum_dbfs;
    std::vector<Reception> receptions; // Bounded most recent first.
    std::vector<FrequencySummary> frequencies;
    std::vector<PositionFix> track;
    std::vector<LaneHealth> lane_health;
    uint64_t spectrum_tiles = 0, spectrum_events = 0, clipped_samples = 0;
    uint32_t spectrum_fft_size = 4096;
    double spectrum_bin_width_hz = 0, spectrum_enbw_hz = 0;
    double background_dbfs = -180;
    std::vector<SpectrumEvent> recent_spectrum_events;
    uint64_t spectrum_bursts = 0;
    std::vector<SpectrumBurst> recent_spectrum_bursts;
    DiscoveryStatus discovery;
    std::vector<WaveformObservation> waveforms; // Bounded recent observations.
};

struct ExportOptions {
    bool include_content = false;
    bool include_receiver_positions = false;
    unsigned coordinate_decimals = 3;
    bool include_provenance = false;
};

// Configuration metadata only. Key bytes never leave Engine through this view.
struct KeyRecordInfo {
    size_t slot = 0;
    std::string label;
    std::string channel_name;
    bool restrict_channel_name = false;
    bool configured = false;
};

class Engine {
public:
    // Consumer-paced synthetic input is for offline correctness fixtures only.
    // It preserves RF sample timing but does not establish real-time throughput.
    // USB callbacks always retain their nonblocking, loss-accounting behavior.
    enum class SyntheticPacing { Realtime, ConsumerPaced };
    explicit Engine(SyntheticPacing synthetic_pacing = SyntheticPacing::Realtime);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    bool start(const ReceiverConfig&, bool explicit_hardware_permission, std::string& error);
    void stop();
    Snapshot snapshot() const;
    // Record slots are independent of RF lanes. Every live frame uses the full
    // explicitly configured keyring; records cannot change during reception.
    bool set_channel_key(size_t slot, const std::string& channel_name,
                         const std::string& key_input, std::string& error);
    bool set_survey_key(size_t slot, const std::string& label,
                        const std::string& key_input, std::string& error);
    bool has_channel_key(size_t slot, const std::string& channel_name) const;
    bool has_survey_key(size_t slot) const;
    size_t configured_key_count() const;
    std::vector<KeyRecordInfo> key_records() const;
    void clear_keys();
    void set_fixed_position(double latitude, double longitude, std::optional<double> altitude = {});
    void clear_position();
    bool connect_gps(const std::string& serial_path, unsigned baud, std::string& error);
    void disconnect_gps();
    GpsConnectionStatus gps_connection_status() const;
    bool open_session(const std::string& path, std::string& error);
    // Commits processed measurements through the recording worker before
    // reporting success. Memory-only history cannot be saved retroactively.
    bool save_session(std::string& error);
    // Copies a consistent committed SQLite snapshot to a new private local
    // file. Historical files are never opened for writing or overwritten.
    bool save_session_copy(const std::string& new_path, std::string& error);
    // Stops and finalizes first, then clears results while retaining receiver,
    // GPS and explicit key setup. Only explicit consent may discard memory-only
    // measurements; recording failures leave the existing session available.
    bool new_session(std::string& error, bool discard_unrecorded = false);
    bool export_session(const std::string& path, const ExportOptions&, std::string& error) const;
    bool export_report(const std::string& path, const ReportOptions&, std::string& error) const;
    bool analyze_survey(const SurveyQuery&, SurveyAnalysis&, std::string& error) const;
    static std::string version();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// maximum_frames bounds synthetic/passive UI smoke tests. Prepared hardware
// setup never starts reception until consent is explicitly given in the UI.
int run_desktop(Engine&, int maximum_frames = 0, bool auto_demo = false,
                const ReceiverConfig* launch_config = nullptr,
                double duration_seconds = 0, bool prepare_only = false, bool until_stopped = false,
                const std::string& settings_directory = {});
}
