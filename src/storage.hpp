// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ovmesh/engine.hpp"
#include <sqlite3.h>
#include <functional>
#include <deque>
#include <string_view>

namespace ovmesh {
// Approved RF aggregates only. Receiver position is associated at window end;
// it does not locate every signal or the transmitter within this interval.
struct SurveyWindow {
    uint64_t id = 0;
    double utc_start_seconds = 0;
    double utc_end_seconds = 0;
    double elapsed_start_seconds = 0;
    double elapsed_end_seconds = 0;
    std::vector<FrequencySummary> frequencies;
    std::optional<PositionFix> receiver_position;
};
class SessionStore {
public:
    SessionStore() = default;
    ~SessionStore();
    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;
    void create(const std::string& path, const ReceiverConfig&, const std::string& session_id);
    void open_readonly(const std::string& path);
    void append(const Reception&);
    void append(const PositionFix&);
    void append(const SurveyWindow&);
    void append(const SpectrumTile&);
    void append(const SpectrumEvent&);
    void append(const CoverageGap&);
    // Repeated ids update an observation after overlapping-subband association.
    // These records contain waveform evidence only, never bytes or packet airtime.
    void append(const WaveformObservation&);
    void append(const DiscoveryGap&);
    void update(const Snapshot&, bool final = false);
    Snapshot read() const;
    void save_copy(const std::string& new_path) const;
    SurveyAnalysis analyze(const SurveyQuery&) const;
    void export_csv(const std::string& path, const ExportOptions&) const;
    void export_geojson(const std::string& path, const ExportOptions&) const;
    // Streaming visitors validate every retained record without UI limits. Nest
    // them inside with_read_snapshot for a consistent multi-table report.
    void with_read_snapshot(const std::function<void()>&) const;
    void visit_tiles(const std::function<void(const SpectrumTile&)>&) const;
    void visit_positions(const std::function<void(const PositionFix&)>&) const;
    void visit_waveforms(const std::function<void(const WaveformObservation&)>&) const;
    void visit_receptions(const std::function<void(const Reception&)>&) const;
    void visit_gaps(const std::function<void(const CoverageGap&)>&) const;
    void write_report_file(const std::string& path,
        const std::function<void(const std::function<void(std::string_view)>&)>& generator) const;
    bool is_open() const { return db_ != nullptr; }
    int schema_version() const noexcept { return schema_version_; }
private:
    sqlite3* db_ = nullptr;
    // Versions 1–5 remain read-only legacy formats. Never migrate historical files.
    int schema_version_ = 0;
    bool readonly_ = true, pending_ = false;
    sqlite3_stmt* tile_insert_ = nullptr;
    ReceiverConfig recorded_config_;
    uint64_t previous_tile_end_ = 0, previous_tile_id_ = 0;
    double previous_tile_elapsed_end_ = 0;
    struct PowerBlock {
        uint64_t id = 0, first_sample = 0, end_sample = 0, frames = 0;
        double utc_start = 0, utc_end = 0, elapsed_start = 0, elapsed_end = 0;
        double first_center = 0, bin_width = 0;
        std::vector<double> power_sum;
        std::vector<float> peak;
        bool dirty = false;
    } power_block_;
    struct SavedFix { PositionFix fix; int64_t id; };
    std::deque<SavedFix> recent_fixes_;
    int64_t ensure_fix(const PositionFix&);
    void save_power_block();
    void begin_batch();
    void execute(const char* sql) const;
    void configure(bool readonly);
};
std::string csv_text(const std::string&);
}
