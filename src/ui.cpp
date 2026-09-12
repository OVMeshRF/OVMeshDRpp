// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/engine.hpp"
#include "ovmesh/licenses.hpp"
#include "ovmesh/file_chooser.hpp"
#include "ovmesh/gps_discovery.hpp"
#include "ovmesh/preferences.hpp"
#include "ovmesh/report.hpp"
#include "ovmesh/meshtastic_presets.hpp"
#include "session_disk.hpp"
#include "geographic_plot.hpp"
#include "desktop_assets.hpp"
#include "occupancy_scale.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <stdexcept>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <future>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ovmesh {
namespace {

// Windows CRT signal handlers reset on invocation; use the UI Stop action there.
#ifdef _WIN32
constexpr bool external_stop_supported = false;
#else
constexpr bool external_stop_supported = true;
#endif
// SIGINT is a stop request only in the explicitly selected POSIX operator mode.
// The handler never calls engine/UI code; the event loop handles the request.
volatile std::sig_atomic_t desktop_stop_requested = 0;
void desktop_stop_signal(int) { desktop_stop_requested = 1; }
class DesktopStopSignal {
public:
    explicit DesktopStopSignal(bool enabled) : enabled_(enabled && external_stop_supported) {
        if (enabled_) {
            desktop_stop_requested = 0;
            previous_ = std::signal(SIGINT, desktop_stop_signal);
            if (previous_ == SIG_ERR) throw std::runtime_error("Unable to install receiver stop control");
        }
    }
    ~DesktopStopSignal() { if (enabled_) std::signal(SIGINT, previous_); }
private:
    bool enabled_;
    void (*previous_)(int) = SIG_DFL;
};

constexpr ImVec4 accent{0.25f, 0.84f, 0.75f, 1.0f};
constexpr ImVec4 secondary{0.41f, 0.69f, 1.0f, 1.0f};
constexpr ImVec4 muted{0.66f, 0.71f, 0.78f, 1.0f};
constexpr ImVec4 amber{0.96f, 0.73f, 0.37f, 1.0f};
constexpr ImVec4 red{1.0f, 0.48f, 0.53f, 1.0f};
// Bounded visual history, independent of the current panel height. These are
// displayed spectrum updates, not a lossless or uniformly timed RF recording.
constexpr size_t waterfall_rows = 1024;
constexpr float waterfall_row_pitch = 2.f;
// Release policy: the desktop is an RF energy survey tool until range-wide
// LoRa processing is qualified. Retain the backend for deliberate development
// checks, but neither saved preferences nor desktop launch flags can enable it.
constexpr bool desktop_lora_enabled = false;

// macOS's supported system libc++ does not yet expose std::jthread. Keep the
// same cancel-and-join ownership with standard thread/CV facilities instead.
// This worker never touches windowing APIs and survives a stalled render loop.
class ReceiveDeadline {
public:
    ReceiveDeadline(Engine& engine, std::atomic<bool>& expired,
                    std::chrono::steady_clock::time_point deadline)
        : worker_([this, &engine, &expired, deadline] {
            std::unique_lock lock(mutex_);
            if (wake_.wait_until(lock, deadline, [this] { return cancelled_; })) return;
            expired.store(true);
            lock.unlock();
            engine.stop();
        }) {}
    ~ReceiveDeadline() { cancel_and_join(); }
    ReceiveDeadline(const ReceiveDeadline&) = delete;
    ReceiveDeadline& operator=(const ReceiveDeadline&) = delete;
    void cancel_and_join() {
        { std::lock_guard lock(mutex_); cancelled_ = true; }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable wake_;
    bool cancelled_ = false;
    std::thread worker_;
};

template <size_t N> void copy_text(std::array<char, N>& destination, const std::string& value) {
    std::snprintf(destination.data(), destination.size(), "%s", value.c_str());
}

template <typename T> void erase_secret(T& value) {
    // Keep erasure observable even in an optimized build. This clears our input
    // copies; it is not a claim that GUI/OS memory has never held key material.
    volatile char* bytes = value.data();
    for (size_t i = 0; i < value.size(); ++i) bytes[i] = 0;
}

std::string clock_text(double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) return "--:--:--";
    const auto stamp = static_cast<std::time_t>(seconds);
    std::tm time{};
#if defined(_WIN32)
    gmtime_s(&time, &stamp);
#else
    gmtime_r(&stamp, &time);
#endif
    std::array<char, 32> text{};
    std::strftime(text.data(), text.size(), "%H:%M:%S UTC", &time);
    return text.data();
}

// UTC is stated by the table heading/caller. Keep the complete calendar date
// for historical and overnight surveys; never replace missing time with now.
std::string timestamp_text(double seconds) {
    constexpr double year_10000 = 253402300800.0;
    if (!std::isfinite(seconds) || seconds <= 0 || seconds >= year_10000)
        return "Unavailable";
    const auto milliseconds = static_cast<int64_t>(std::floor(seconds * 1000));
    const auto whole = milliseconds / 1000;
    if (whole >= static_cast<int64_t>(year_10000)) return "Unavailable";
    if (static_cast<long double>(whole) > static_cast<long double>(std::numeric_limits<std::time_t>::max()))
        return "Unavailable";
    const auto stamp = static_cast<std::time_t>(whole);
    std::tm time{};
#if defined(_WIN32)
    if (gmtime_s(&time, &stamp) != 0) return "Unavailable";
#else
    if (!gmtime_r(&stamp, &time)) return "Unavailable";
#endif
    std::array<char, 32> date{};
    if (!std::strftime(date.data(), date.size(), "%Y-%m-%d %H:%M:%S", &time)) return "Unavailable";
    const int millis = static_cast<int>(milliseconds % 1000);
    std::array<char, 40> text{};
    std::snprintf(text.data(), text.size(), "%s.%03d", date.data(), millis);
    return text.data();
}

void reception_time_tooltip(double utc, double elapsed, bool waveform) {
    if (!ImGui::IsItemHovered()) return;
    ImGui::BeginTooltip();
    ImGui::Text("%s UTC", timestamp_text(utc).c_str());
    if (std::isfinite(elapsed) && elapsed >= 0) ImGui::Text("Elapsed session time: %.6f s", elapsed);
    ImGui::TextUnformatted(waveform ? "Host-estimated waveform delimiter time; not packet completion."
                                   : "Host-estimated reception time; not sender-reported time.");
    ImGui::TextUnformatted("Millisecond display does not imply GPS-synchronized accuracy.");
    ImGui::EndTooltip();
}

std::string duration_text(double seconds) {
    const auto total = static_cast<unsigned long long>(std::max(0.0, seconds));
    std::array<char, 48> text{};
    std::snprintf(text.data(), text.size(), "%02llu:%02llu:%02llu", total / 3600,
                  (total / 60) % 60, total % 60);
    return text.data();
}

double ratio(double numerator, double denominator) {
    return denominator > 0 ? std::clamp(numerator / denominator, 0.0, 1.0) : 0.0;
}

void label(const char* text) {
    ImGui::Spacing();
    ImGui::TextColored(muted, "%s", text);
    ImGui::Spacing();
}

void help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void wrapped(const char* text, ImVec4 color = muted) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void pill(const char* text, ImVec4 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void setup_style() {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.ChildRounding = 7;
    style.FrameRounding = 4;
    style.PopupRounding = 6;
    style.GrabRounding = 4;
    style.ScrollbarRounding = 8;
    style.TabRounding = 4;
    style.WindowPadding = {15, 13};
    style.FramePadding = {8, 6};
    style.ItemSpacing = {9, 7};
    style.CellPadding = {8, 7};
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 1;
    style.Colors[ImGuiCol_WindowBg] = {0.045f, 0.058f, 0.075f, 1};
    style.Colors[ImGuiCol_ChildBg] = {0.062f, 0.079f, 0.101f, 1};
    style.Colors[ImGuiCol_PopupBg] = {0.075f, 0.098f, 0.12f, 1};
    style.Colors[ImGuiCol_Border] = {0.14f, 0.19f, 0.23f, 1};
    style.Colors[ImGuiCol_FrameBg] = {0.11f, 0.15f, 0.19f, 1};
    style.Colors[ImGuiCol_FrameBgHovered] = {0.15f, 0.22f, 0.26f, 1};
    style.Colors[ImGuiCol_FrameBgActive] = {0.18f, 0.29f, 0.32f, 1};
    style.Colors[ImGuiCol_Button] = {0.12f, 0.25f, 0.28f, 1};
    style.Colors[ImGuiCol_ButtonHovered] = {0.16f, 0.38f, 0.39f, 1};
    style.Colors[ImGuiCol_ButtonActive] = {0.20f, 0.47f, 0.44f, 1};
    style.Colors[ImGuiCol_Header] = {0.12f, 0.27f, 0.29f, 1};
    style.Colors[ImGuiCol_HeaderHovered] = {0.15f, 0.33f, 0.36f, 1};
    style.Colors[ImGuiCol_HeaderActive] = {0.17f, 0.40f, 0.40f, 1};
    style.Colors[ImGuiCol_CheckMark] = accent;
    style.Colors[ImGuiCol_SliderGrab] = accent;
    style.Colors[ImGuiCol_PlotHistogram] = accent;
    style.Colors[ImGuiCol_TableHeaderBg] = {0.10f, 0.14f, 0.18f, 1};
    style.Colors[ImGuiCol_TableRowBgAlt] = {0.10f, 0.15f, 0.19f, 0.45f};
    style.Colors[ImGuiCol_Text] = {0.86f, 0.91f, 0.94f, 1};
    style.Colors[ImGuiCol_TextDisabled] = muted;
}

ImU32 heat_color(float dbfs, float floor, float ceiling) {
    const float value = std::clamp((dbfs - floor) / std::max(1.0f, ceiling - floor), 0.0f, 1.0f);
    const std::array<ImVec4, 6> stops{{
        {0.02f, 0.04f, 0.09f, 1}, {0.07f, 0.17f, 0.35f, 1},
        {0.10f, 0.39f, 0.53f, 1}, {0.21f, 0.71f, 0.66f, 1},
        {0.89f, 0.76f, 0.37f, 1}, {0.99f, 0.96f, 0.79f, 1}}};
    const float scaled = value * 5;
    const int index = std::min(4, static_cast<int>(scaled));
    const float f = scaled - static_cast<float>(index);
    const auto& a = stops[static_cast<size_t>(index)];
    const auto& b = stops[static_cast<size_t>(index + 1)];
    return ImGui::ColorConvertFloat4ToU32({a.x + (b.x - a.x) * f,
        a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f, 1});
}

enum class OccupancyScale { RevealLowActivity, Linear };
enum class FilePickerPurpose { None, SaveSurvey, OpenSurvey, Export, SaveCopy, WaveformImage };
std::string path_utf8(const std::filesystem::path& path);
struct FilePickerState {
    FilePickerPurpose purpose = FilePickerPurpose::None;
    bool request_open = false;
    bool show_hidden = false;
    int format = 0;
    std::string directory;
    std::array<char, 1024> filename{};
    FileBrowserListing listing;
    std::vector<size_t> visible_entries;
    std::vector<std::string> roots;
    std::string error;
};

enum class PositionViewMode { Stationary, Mobile };

struct FrequencyRange {
    double lower_hz = 0, upper_hz = 0;
};

struct FrequencyChartSelection {
    double click_width_hz = 125000;
    std::optional<FrequencyRange> highlighted;
    std::optional<double> anchor_fraction;
    std::optional<FrequencyRange> requested;
};

struct PendingAnalysis {
    SurveyQuery query;
    std::string session_id;
    bool reveal_time_plot = false;
};

struct CompletedAnalysis {
    PendingAnalysis request;
    SurveyAnalysis analysis;
    bool ok = false;
    std::string error;
};

struct BurstListState {
    bool paused = false, show_fragments = false, was_running = false;
    double refreshed = -1;
    std::string session;
    uint64_t count = 0;
    std::vector<SpectrumBurst> bursts;
    std::vector<SpectrumEvent> fragments;
    void update(const Snapshot& snapshot, double now) {
        if (session != snapshot.session_id) {
            session = snapshot.session_id; refreshed = -1; paused = false;
            bursts.clear(); fragments.clear(); count = 0;
        }
        if (!paused && (refreshed < 0 || now - refreshed >= 1 || (was_running && !snapshot.running))) {
            bursts = snapshot.recent_spectrum_bursts; fragments = snapshot.recent_spectrum_events;
            count = snapshot.spectrum_bursts; refreshed = now;
            const auto latest_first = [](const auto& a, const auto& b) {
                return a.elapsed_start_seconds > b.elapsed_start_seconds ||
                    (a.elapsed_start_seconds == b.elapsed_start_seconds && a.id > b.id);
            };
            std::sort(bursts.begin(),bursts.end(),latest_first);
            std::sort(fragments.begin(),fragments.end(),latest_first);
        }
        was_running = snapshot.running;
    }
};

int receiver_source_index(const ReceiverConfig& config) {
    return config.synthetic ? 0 : config.hardware_receiver == HardwareReceiver::Rak5146 ? 3 :
        config.hardware_receiver == HardwareReceiver::RtlSdr ? 2 : 1;
}

bool is_concentrator(const ReceiverConfig& config) { return receiver_source_index(config) == 3; }

std::string desktop_acquisition_log(const ReceiverConfig& applied, size_t key_records,
                                    double duration_seconds, double deadline_utc_seconds) {
    std::ostringstream out;
    out << "Started " << receiver_source_name(applied) << " desktop nominal_center_hz=" << applied.center_hz
        << " tuning_offset_hz=" << applied.tuning_offset_hz << " tuned_center_hz=" << tuned_center_hz(applied)
        << " survey_span_hz=" << applied.survey_span_hz;
    const bool rak = is_concentrator(applied);
    const bool packets = rak && std::any_of(applied.concentrators.boards.begin(), applied.concentrators.boards.end(),
        [](const auto& board) { return board.packets_enabled; });
    if (rak) {
        out << " boards=" << applied.concentrators.boards.size() << " scan_enabled=" << applied.concentrators.scan_enabled
            << " scan_step_hz=" << applied.concentrators.scan_step_hz << " samples_per_scan=" << applied.concentrators.scan_samples
            << " decode_enabled=" << applied.concentrators.decode_enabled;
    } else {
        out << " sample_rate=" << applied.sample_rate << " lna_gain_db=" << applied.lna_gain
            << " vga_gain_db=" << applied.vga_gain << " rf_amplifier=" << applied.amplifier
            << " rtl_gain_tenths_db=" << applied.rtl_gain_tenths_db << " rtl_auto_gain=" << applied.rtl_auto_gain << " antenna_bias=0";
    }
    out << std::fixed << std::setprecision(3) << " duration_seconds=" << duration_seconds
        << " deadline_utc_seconds=" << deadline_utc_seconds << '\n';
    if (rak) {
        for (size_t i = 0; i < applied.concentrators.boards.size(); ++i) {
            const auto& board = applied.concentrators.boards[i];
            out << "Desktop configured board=" << i + 1 << " packets_enabled=" << board.packets_enabled
                << " frequency_hz=" << board.frequency_hz << " bandwidth_hz=" << board.bandwidth_hz
                << " sf=" << board.spreading_factor << " sync_word=0x" << std::hex << board.sync_word << std::dec << '\n';
        }
    } else {
        for (size_t i = 0; i < applied.lanes.size(); ++i) {
            const auto& lane = applied.lanes[i];
            out << "Desktop started lane=" << i + 1 << " enabled=" << lane.enabled << " frequency_hz=" << lane.frequency_hz
                << " bandwidth_hz=" << lane.bandwidth_hz << " sf=" << static_cast<unsigned>(lane.spreading_factor)
                << " cr_denominator=" << static_cast<unsigned>(lane.coding_rate) << '\n';
        }
    }
    out << "Desktop keyring configured_records=" << key_records << " waveform_discovery=" << applied.discover_lora
        << " automatic_decoder_dispatch=" << applied.automatic_decode << " decoding_scope="
        << (rak ? !packets ? "paused_spectrum_only" : applied.concentrators.decode_enabled ? "configured_hardware_profiles" : "disabled_packet_metadata_only"
                : applied.automatic_decode ? "discovered_waveforms" : applied.lanes.empty() ? "paused_spectrum_only" : "selected_profiles") << '\n';
    return out.str();
}

bool receiver_source_available(int source, const Snapshot& snapshot) {
    return source == 0 || (source == 1 && snapshot.hardware_available) || (source == 2 && snapshot.rtl_sdr_available) ||
        (source == 3 && snapshot.rak5146_available);
}

struct DesktopState {
    SessionDiskMonitor session_disk;
    BurstListState burst_list;
    ReceiverConfig config;
    int source = 0;
    int selected_lane = 0;
    int selected_key_record = 0;
    int key_mode = 1;
    std::string session_path;
    std::array<char, 160> session_title{};
    std::string reopen_path;
    std::string export_path;
    FilePickerState file_picker;
    std::function<void(const std::string&)> file_chosen;
    struct OperationResult { bool ok = false; std::string message; };
    std::future<OperationResult> operation;
    std::function<void()> after_operation;
    std::string operation_label;
    bool operation_busy() const { return operation.valid(); }
    size_t cached_key_count = 0;
    size_t key_count(Engine& engine) {
        if (!operation_busy()) cached_key_count = engine.configured_key_count();
        return cached_key_count;
    }
    void begin_operation(const std::string& text, std::function<OperationResult()> work,
                         std::function<void()> completed = {}) {
        if (operation_busy()) return;
        operation_label = text;
        after_operation = std::move(completed);
        operation = std::async(std::launch::async, [work = std::move(work)] {
            try { return work(); }
            catch (const std::exception& e) { return OperationResult{false, e.what()}; }
        });
    }
    void finish_operation() {
        if (!operation_busy() || operation.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        auto result = operation.get();
        feedback(result.ok, result.message);
        if (result.ok && after_operation) after_operation();
        after_operation = {}; operation_label.clear();
    }
    void clear_views() {
        waterfall.clear(); last_spectrum = 0; last_session.clear(); selected.reset(); show_detail = false;
        analysis_loaded = false; analysis_session.clear(); analysis = {}; analyzed_query = {}; survey_query = {};
        summary_selection = {}; detail_selection = {}; pending_analysis.reset(); selected_observation.reset();
        query_lower_mhz = query_upper_mhz = 0; burst_list = {}; freeze_waterfall = false;
        waveform_rows.clear(); waveform_refresh = -1; last_analysis_refresh = -1;
        focus_analysis = false; focus_live = true;
    }
    bool spectrum_only = !desktop_lora_enabled;
    bool decode_enabled = desktop_lora_enabled;
    bool public_meshtastic_key_enabled = false; // Passive construction never adds a key.
    bool mixed_fonts = true;
    ImFont* sans_font = nullptr;
    ImFont* mono_font = nullptr;
    int settings_page = desktop_lora_enabled ? 0 : 1;
    bool show_diagnostics = false;
    bool show_analysis_details = false;
    int analysis_view = 0;
    bool new_requested = false;
    bool close_requested = false;
    bool close_approved = false;
    std::string pending_open_path;
    bool capture_requested = false;
    ImVec2 capture_origin{}, capture_size{};
    std::vector<uint8_t> capture_pixels;
    uint32_t capture_width = 0, capture_height = 0;
    std::string capture_path;
    std::string copy_path;
    std::vector<WaveformObservation> waveform_rows;
    double waveform_refresh = -1;
    std::string waveform_session;
    double last_analysis_refresh = -1;
    bool last_analysis_requested_while_running = false;
    PreferencePaths preference_locations;
    DesktopPreferences preferences;
    GpsDiscovery gps_devices;
    ConcentratorDiscovery concentrator_devices;
    bool concentrator_inventory_loaded = false;
    std::optional<size_t> selected_gps;
    bool preferences_active = false;
    bool preferences_ready = false;
    bool gps_enabled = true;
    bool focus_serial_gps = false;
    bool recording_path_used = false;
    std::string preferences_error;
    std::array<char, 160> device_serial{};
    std::array<char, 256> antenna_description{};
    std::array<char, 256> receiver_description{};
    std::array<char, 1024> survey_notes{};
    std::array<char, 512> gps_path{};
    std::array<char, 160> channel_name{};
    std::array<char, 81> key_label{};
    std::array<char, 65> key_input{};
    std::array<char, 160> filter{};
    ExportOptions export_options;
    int export_kind = 0; // Five CSV reports, detailed archive, narrative HTML.
    SurveyQuery export_query;
    std::string export_session_id;
    bool export_from_analysis = false;
    double export_time_bucket_seconds = 60;
    double export_geographic_cell_m = 100;
    std::string notice;
    bool notice_error = false;
    bool notice_warning = false;
    bool authorize_hardware = false;
    bool authorize_keys = false;
    bool show_settings = false;
    bool show_licenses = false;
    int selected_license = 0;
    bool show_keys = false;
    bool show_export = false;
    bool only_classified = false;
    bool freeze_waterfall = false;
    bool save_session = false;
    bool show_detail = false;
    bool chart_click_tune = false;
    bool timed_run = false;
    bool manual_stop = false;
    bool managed_started = false;
    bool prepared_run = false;
    bool passive_smoke = false;
    double timed_duration = 0;
    std::function<void()> after_start;
    std::chrono::steady_clock::time_point deadline{};
    float display_floor = -100;
    float display_ceiling = -15;
    float ui_scale = 1;
    // Logical pixel heights: resize without changing measurement or recording.
    float spectrum_height = 110;
    float waterfall_height = 220;
    ImVec2 spectrum_grabber{}, waterfall_grabber{};
    uint64_t waterfall_center_hz = 0;
    uint32_t waterfall_span_hz = 0;
    int gps_baud = 9600;
    double latitude = 0;
    double longitude = 0;
    double altitude = 0;
    bool have_altitude = false;
    uint64_t last_spectrum = 0;
    std::string last_session;
    std::deque<std::vector<float>> waterfall;
    std::optional<Reception> selected;
    SurveyQuery survey_query;
    SurveyQuery analyzed_query;
    SurveyAnalysis analysis;
    OccupancyScale occupancy_scale = OccupancyScale::RevealLowActivity;
    PositionViewMode position_view_mode = PositionViewMode::Stationary;
    bool use_center_guard = true;
    bool analysis_loaded = false;
    bool focus_analysis = false;
    bool focus_live = false;
    double query_lower_mhz = 0, query_upper_mhz = 0;
    double query_width_khz = 125;
    int width_choice = 1;
    std::optional<size_t> selected_observation;
    std::string analysis_session;
    FrequencyChartSelection summary_selection, detail_selection;
    std::optional<PendingAnalysis> pending_analysis;
    std::future<CompletedAnalysis> analysis_operation;
    bool analysis_busy() const { return pending_analysis.has_value() || analysis_operation.valid(); }
    bool reveal_time_plot = false;

    DesktopState() {
        config.lanes.clear(); // Passive construction; ordinary startup arms the saved defaults.
        enforce_spectrum_only_policy();
        copy_text(session_title, "RF survey");
        copy_text(channel_name, "LongFast");
        copy_text(key_label, "Survey key");
    }
    ~DesktopState() { erase_secret(key_input); }

    void enforce_spectrum_only_policy() {
        if (desktop_lora_enabled) return;
        spectrum_only = true;
        decode_enabled = false;
        public_meshtastic_key_enabled = false;
        config.discover_lora = false;
        config.automatic_decode = false;
        config.lanes.clear();
        config.concentrators.decode_enabled = false;
        for (auto& board : config.concentrators.boards) board.packets_enabled = false;
        show_keys = false;
        show_detail = false;
    }

    void feedback(bool ok, const std::string& message) {
        notice = message;
        notice_error = !ok;
        notice_warning = false;
    }
    void use_launch_detection(const ReceiverConfig& requested) {
        if (!desktop_lora_enabled) { enforce_spectrum_only_policy(); return; }
        if (is_concentrator(requested)) {
            spectrum_only = std::none_of(requested.concentrators.boards.begin(), requested.concentrators.boards.end(),
                [](const auto& board) { return board.packets_enabled; });
            decode_enabled = requested.concentrators.decode_enabled;
        } else {
            spectrum_only = !requested.discover_lora && requested.lanes.empty() && !requested.automatic_decode;
            decode_enabled = !requested.lanes.empty() || requested.automatic_decode;
        }
    }
    bool automatic_decode_requested() const {
        return desktop_lora_enabled && source != 3 && config.discover_lora && decode_enabled && !spectrum_only &&
            (preferences_active || config.automatic_decode);
    }
    template<class DiscoverGps = decltype(&discover_gps_devices)>
    void refresh_gps(DiscoverGps discover = discover_gps_devices) {
        gps_devices = discover();
        selected_gps = gps_devices.error.empty() ? select_gps_device(gps_devices.devices, preferences.gps_device_id) : std::nullopt;
    }
    template<class Discover = decltype(&discover_concentrator_devices)>
    void refresh_concentrators(Discover discover = discover_concentrator_devices) {
        concentrator_devices = discover();
        concentrator_inventory_loaded = true;
    }
    // Resolve a saved USB identity or an explicit command-line path. A missing
    // or duplicate identity is never replaced by an arbitrary matching port.
    template<class Discover = decltype(&discover_concentrator_devices)>
    bool resolve_concentrators(Discover discover = discover_concentrator_devices) {
        refresh_concentrators(discover);
        if (!concentrator_devices.error.empty()) { feedback(false, concentrator_devices.error); return false; }
        auto boards = config.concentrators.boards;
        for (size_t i = 0; i < boards.size(); ++i) {
            auto selected = select_concentrator_device(concentrator_devices.devices, boards[i].device_id);
            if (boards[i].device_id.empty() && !boards[i].device_path.empty()) {
                // A path supplied explicitly on the CLI is a selection, not an
                // invitation to probe candidates. Resolve its unique metadata.
                size_t matches = 0;
                for (const auto& device : concentrator_devices.devices) if (device.path == boards[i].device_path) {
                    ++matches;
                    selected = select_concentrator_device(concentrator_devices.devices, device.stable_id);
                }
                if (matches != 1) selected.reset();
            }
            if (!selected) {
                if (boards[i].device_id.empty() && boards[i].device_path.empty())
                    feedback(false, "Select Board " + std::to_string(i + 1) + " in Settings > RAK concentrators, then start reception.");
                else
                    feedback(false, "The selected RAK board is unavailable or ambiguous. Refresh USB candidates in Settings > RAK concentrators and select the board again.");
                return false;
            }
            boards[i].device_path = concentrator_devices.devices[*selected].path;
            boards[i].device_id = concentrator_devices.devices[*selected].stable_id;
            for (size_t j = 0; j < i; ++j) if (boards[j].device_path == boards[i].device_path) {
                feedback(false, "Choose a different USB concentrator for each board."); return false;
            }
        }
        config.concentrators.boards = std::move(boards);
        return true;
    }
    // Updates setup only. Selecting a source never enumerates or opens an SDR.
    void select_receiver(int next_source) {
        enforce_spectrum_only_policy();
        if (next_source < 0 || next_source > 3 || next_source == source) return;
        const bool hardware_changed = next_source != 0 && next_source != source;
        source = next_source;
        config.synthetic = source == 0;
        config.hardware_receiver = source == 3 ? HardwareReceiver::Rak5146 :
            source == 2 ? HardwareReceiver::RtlSdr : HardwareReceiver::HackRf;
        if (hardware_changed) {
            device_serial.fill(0);
            config.device_serial.clear();
            config.tuning_offset_hz = 0;
        }
        if (source == 2) {
            config.sample_rate = 2000000;
            config.survey_span_hz = 1500000;
            config.amplifier = false;
            if (config.center_hz < 24000000 || config.center_hz > 1766000000ULL)
                config.center_hz = 906875000;
        } else if (source == 3) {
            concentrator_inventory_loaded = false;
            config.center_hz = 915000000;
            config.survey_span_hz = 26000000;
            config.amplifier = false;
        } else if (config.sample_rate < 8000000 || config.survey_span_hz > config.sample_rate * 4 / 5) {
            config.sample_rate = 16000000;
            config.survey_span_hz = 10000000;
        }
        feedback(true, source == 3 ? "RAK selected: swept 902-928 MHz energy survey. Choose the USB boards in Settings." : source == 2 ?
            "RTL-SDR selected: 1.5 MHz survey at 2 MS/s. Check the center frequency; Offset reset for this receiver." :
            "Receiver selected. Check the survey range and Offset before starting.");
    }
    void prepare_discovery_rate() {
        if (!desktop_lora_enabled) { enforce_spectrum_only_policy(); return; }
        if (source != 2 || spectrum_only || !config.discover_lora) return;
        if (config.sample_rate != 2000000 || config.survey_span_hz > 1500000) {
            config.sample_rate = 2000000;
            config.survey_span_hz = std::min(config.survey_span_hz, 1500000U);
            feedback(true, "RTL-SDR LoRa discovery uses 2 MS/s with up to a 1.5 MHz survey span. Check the displayed range.");
        }
    }
    void persist_preferences() {
        enforce_spectrum_only_policy();
        if (!preferences_ready) return;
        preferences.recording_enabled = save_session;
        preferences.compact_recording = config.compact_recording;
        preferences.gps_enabled = gps_enabled;
        preferences.gps_baud = static_cast<unsigned>(gps_baud);
        preferences.tuning_offset_hz = config.tuning_offset_hz;
        preferences.discover_lora = config.discover_lora;
        preferences.spectrum_only = spectrum_only;
        preferences.decode_enabled = decode_enabled;
        preferences.public_meshtastic_key_enabled = public_meshtastic_key_enabled;
        preferences.mixed_fonts = mixed_fonts;
        preferences.mobile_position_display = position_view_mode == PositionViewMode::Mobile;
        preferences.receiver_source = source == 0 ? DesktopReceiver::Synthetic :
            source == 3 ? DesktopReceiver::Rak5146 : source == 2 ? DesktopReceiver::RtlSdr : DesktopReceiver::HackRf;
        preferences.concentrators = config.concentrators;
        preferences.center_hz = config.center_hz;
        preferences.sample_rate = config.sample_rate;
        preferences.survey_span_hz = config.survey_span_hz;
        preferences.lna_gain = config.lna_gain;
        preferences.vga_gain = config.vga_gain;
        preferences.amplifier = config.amplifier;
        preferences.rtl_gain_tenths_db = config.rtl_gain_tenths_db;
        preferences.rtl_auto_gain = config.rtl_auto_gain;
        try {
            save_preferences(preference_locations, preferences);
            preferences_error.clear();
        } catch (const std::exception& e) {
            preferences_error = std::string("Settings could not be saved: ") + e.what();
        }
    }
    void apply_public_key(Engine& engine) {
        enforce_spectrum_only_policy();
        std::string error;
        if (!engine.set_public_meshtastic_key_enabled(public_meshtastic_key_enabled, error)) feedback(false, error);
    }
    void prepare_recording_file() {
        if (!save_session || (!preferences_ready && session_path.empty())) return;
        try {
            const auto directory = preferences_ready ? preferences.recording_directory :
                path_utf8(std::filesystem::path(std::u8string(session_path.begin(), session_path.end())).parent_path());
            session_path = new_survey_path(directory);
            recording_path_used = false;
        } catch (const std::exception& e) {
            session_path.clear();
            feedback(false, std::string("Choose an available recording folder: ") + e.what());
        }
    }
    void initialize_preferences(const std::string& directory, bool discover = true, bool restore_receiver = true) {
        preferences_active = true;
        save_session = true;
        gps_enabled = true;
        focus_serial_gps = true;
        try {
            preference_locations = preference_paths(directory);
            preferences = load_preferences(preference_locations);
            ensure_preferences_directories(preference_locations);
            preferences_ready = true;
            save_session = preferences.recording_enabled;
            gps_enabled = preferences.gps_enabled;
            gps_baud = static_cast<int>(preferences.gps_baud);
            if (restore_receiver) config.tuning_offset_hz = preferences.tuning_offset_hz;
            // An explicit launch request takes precedence over a saved opt-out.
            config.discover_lora = config.discover_lora || preferences.discover_lora;
            // An explicit detailed-recording launch request overrides the saved
            // default. Merely loading preferences never starts a receiver.
            config.compact_recording = config.compact_recording && preferences.compact_recording;
            spectrum_only = preferences.spectrum_only;
            decode_enabled = preferences.decode_enabled;
            public_meshtastic_key_enabled = preferences.public_meshtastic_key_enabled;
            mixed_fonts = preferences.mixed_fonts;
            position_view_mode = preferences.mobile_position_display ? PositionViewMode::Mobile : PositionViewMode::Stationary;
            if (restore_receiver) {
                source = static_cast<int>(preferences.receiver_source);
                config.synthetic = source == 0;
                config.hardware_receiver = source == 3 ? HardwareReceiver::Rak5146 :
                    source == 2 ? HardwareReceiver::RtlSdr : HardwareReceiver::HackRf;
                config.concentrators = preferences.concentrators;
                config.center_hz = preferences.center_hz;
                config.sample_rate = preferences.sample_rate;
                config.survey_span_hz = preferences.survey_span_hz;
                config.lna_gain = preferences.lna_gain;
                config.vga_gain = preferences.vga_gain;
                config.amplifier = preferences.amplifier;
                config.rtl_gain_tenths_db = preferences.rtl_gain_tenths_db;
                config.rtl_auto_gain = preferences.rtl_auto_gain;
                // Ordinary range-wide discovery needs no assumed channel frequency.
                // Explicit manual profiles are left unchanged; keys are applied separately.
            }
            prepare_recording_file();
            persist_preferences();
        } catch (const std::exception& e) {
            preferences_ready = false;
            preferences_error = std::string("Saved setup unavailable: ") + e.what();
        }
        enforce_spectrum_only_policy();
        if (discover) refresh_gps(); // OS inventory only; no serial/device opens.
    }
    // Keep discovery/receiver operations substitutable for hardware-free checks
    // of this same startup path. Production calls use OS metadata and Engine.
    template<class Receiver, class DiscoverGps = decltype(&discover_gps_devices)>
    bool connect_selected_gps(Receiver& engine, DiscoverGps discover = discover_gps_devices) {
        refresh_gps(discover); // Recheck identity after any USB rearrangement.
        if (!gps_devices.error.empty()) {
            engine.disconnect_gps();
            feedback(false, gps_devices.error);
            return false;
        }
        if (!selected_gps) {
            // Do not keep an earlier serial fix when its identity is no longer
            // available/unambiguous. Engine preserves an explicit manual fix.
            engine.disconnect_gps();
            feedback(false, "GPS unavailable or ambiguous. Choose a receiver in Settings > GPS & location.");
            return false;
        }
        const auto& device = gps_devices.devices[*selected_gps];
        const auto status = engine.gps_connection_status();
        if (status.device_path == device.path && status.state != GpsConnectionState::Disconnected && status.state != GpsConnectionState::ReadError)
            return true;
        engine.disconnect_gps();
        std::string error;
        const bool ok = engine.connect_gps(device.path, static_cast<unsigned>(gps_baud), error);
        feedback(ok, ok ? "GPS connected. Waiting for a current position." : error);
        return ok;
    }
    template<class Receiver, class DiscoverGps = decltype(&discover_gps_devices),
             class DiscoverConcentrators = decltype(&discover_concentrator_devices)>
    void start(Receiver& engine, bool permission, DiscoverGps discover = discover_gps_devices,
               DiscoverConcentrators discover_boards = discover_concentrator_devices) {
        enforce_spectrum_only_policy();
        if (passive_smoke) {
            feedback(false, "Passive UI checks cannot start reception.");
            return;
        }
        if (managed_started || (manual_stop && desktop_stop_requested)) {
            feedback(false, "This managed session cannot be restarted. Its results remain selected for review.");
            return;
        }
        const auto previous = engine.snapshot();
        const bool resuming = !previous.session_id.empty() && !previous.historical;
        config.synthetic = source == 0;
        config.hardware_receiver = source == 3 ? HardwareReceiver::Rak5146 :
            source == 2 ? HardwareReceiver::RtlSdr : HardwareReceiver::HackRf;
        if (source == 3 && !resolve_concentrators(discover_boards)) return;
        if (resuming) {
            session_path = previous.config.session_path;
            save_session = !session_path.empty();
        } else if (preferences_active && save_session && (session_path.empty() || recording_path_used)) prepare_recording_file();
        if (save_session && session_path.empty()) {
            feedback(false, "Choose a recording location before starting. Recording has not been disabled.");
            return;
        }
        config.session_path = save_session ? session_path : "";
        config.session_title = session_title.data();
        config.device_serial = device_serial.data();
        config.antenna_description = antenna_description.data();
        config.receiver_description = receiver_description.data();
        config.survey_notes = survey_notes.data();
        if (prepared_run && config.synthetic) {
            feedback(false, "This prepared session requires its explicitly selected hardware receiver.");
            return;
        }
        std::string error;
        // Ordinary startup may already have connected the recognized GPS.
        // Saved analysis, passive checks and synthetic demos remain isolated.
        std::string gps_warning;
        if (preferences_active && !config.synthetic && gps_enabled && permission && !connect_selected_gps(engine, discover))
            gps_warning = notice;
        if (preferences_active && config.synthetic && engine.gps_connection_status().state != GpsConnectionState::Disconnected) engine.disconnect_gps();
        // Storage can be created before USB startup fails. Preserve that file
        // and allocate a fresh destination on the next ordinary-session retry.
        recording_path_used = save_session;
        auto effective = config;
        if (spectrum_only) effective.discover_lora = false;
        if (spectrum_only || !decode_enabled) effective.lanes.clear();
        effective.automatic_decode = automatic_decode_requested();
        if (source == 3) {
            effective.discover_lora = false; effective.lanes.clear();
            effective.concentrators.decode_enabled = decode_enabled && !spectrum_only;
            if (spectrum_only) for (auto& board : effective.concentrators.boards) board.packets_enabled = false;
        }
        // Legacy profiles outside a newly chosen survey range must not prevent
        // range-wide surveying. Preserve their settings for a later session.
        const double lower = double(effective.center_hz) - effective.survey_span_hz * .5;
        const double upper = double(effective.center_hz) + effective.survey_span_hz * .5;
        std::erase_if(effective.lanes, [&](const LaneConfig& lane) {
            return double(lane.frequency_hz) - lane.bandwidth_hz * .5 < lower ||
                   double(lane.frequency_hz) + lane.bandwidth_hz * .5 > upper ||
                   (lane.bandwidth_hz != 0 && effective.sample_rate % (uint64_t(lane.bandwidth_hz) * 4) != 0);
        });
        const bool ok = engine.start(effective, permission, error);
        feedback(ok, ok ? (config.synthetic ? "Synthetic reception started. No USB device is accessed."
                                             : "Receive-only hardware session started.") : error);
        if (ok) {
            if (!gps_warning.empty()) {
                feedback(true, "Reception started without GPS. " + gps_warning);
                notice_warning = true;
            }
            recording_path_used = save_session;
            if (after_start) {
                try { after_start(); }
                catch (const std::exception& e) {
                    engine.stop();
                    feedback(false, std::string("Receiver timer could not start: ") + e.what());
                    return;
                }
            }
            // The live waterfall is transient. Keep it on same-range resume;
            // spectrum_view clears it if the acquisition axis actually changes.
            analysis_loaded = false;
            selected_observation.reset();
            selected.reset();
            last_spectrum = 0;
        }
    }
};

std::string path_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

void filter_file_entries(FilePickerState& picker) {
    picker.visible_entries.clear();
    for (size_t i = 0; i < picker.listing.entries.size(); ++i) {
        const auto& name = picker.listing.entries[i].name;
        if (!picker.show_hidden && !name.empty() && name.front() == '.') continue;
        picker.visible_entries.push_back(i);
    }
}

void browse_folder(FilePickerState& picker, const std::string& directory) {
    auto listing = list_local_directory(directory);
    if (!listing.error.empty()) {
        picker.error = listing.error;
        return;
    }
    picker.directory = directory;
    picker.listing = std::move(listing);
    picker.error.clear();
    if (picker.purpose == FilePickerPurpose::OpenSurvey) picker.filename.fill(0);
    filter_file_entries(picker);
}

void begin_file_picker(DesktopState& ui, FilePickerPurpose purpose, const std::string& current,
                       const std::string& survey_path = {}) {
    auto& picker = ui.file_picker;
    picker.purpose = purpose;
    picker.request_open = true;
    picker.error.clear();
    picker.filename.fill(0);
    try {
        picker.roots = file_browser_roots();
        // Reuse the last browsed folder, then the current selection/session.
        const auto& initial = current.empty() ? survey_path : current;
        if (!initial.empty()) browse_folder(picker, path_utf8(std::filesystem::path(std::u8string(initial.begin(), initial.end())).parent_path()));
        else if (picker.directory.empty()) browse_folder(picker, file_browser_home());
        else browse_folder(picker, picker.directory);
        if (picker.directory.empty()) browse_folder(picker, file_browser_home());
        std::string filename = purpose == FilePickerPurpose::Export ? (ui.export_kind == 6 ? "survey-analysis.html" : "survey.csv") :
            purpose == FilePickerPurpose::WaveformImage ? "waveform.png" :
            purpose == FilePickerPurpose::SaveCopy ? "survey-copy.sqlite" : "survey.sqlite";
        if (purpose == FilePickerPurpose::OpenSurvey) filename.clear();
        if (!current.empty()) filename = path_utf8(std::filesystem::path(std::u8string(current.begin(), current.end())).filename());
        if (filename.size() >= picker.filename.size()) picker.error = "This filename is too long for the chooser. Choose a shorter name.";
        else copy_text(picker.filename, filename);
        auto extension = path_utf8(std::filesystem::path(std::u8string(filename.begin(), filename.end())).extension());
        for (auto& c : extension) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        picker.format = extension == ".geojson" ? 1 : 0;
    } catch (const std::exception& e) { picker.error = e.what(); }
}

void selected_file_field(const char* id, const char* hint, std::string& path) {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint(id, hint, path.data(), path.size() + 1, ImGuiInputTextFlags_ReadOnly);
    if (!path.empty() && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(path.c_str());
        ImGui::EndTooltip();
    }
}

void file_picker_dialog(DesktopState& ui, const Snapshot& snapshot) {
    auto& picker = ui.file_picker;
    if (picker.request_open) {
        ImGui::OpenPopup("Choose a local file");
        picker.request_open = false;
    }
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize({std::min(760.0f, viewport->WorkSize.x - 30),
                              std::min(600.0f, viewport->WorkSize.y - 30)}, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    bool open = true;
    if (!ImGui::BeginPopupModal("Choose a local file", &open, ImGuiWindowFlags_NoCollapse)) return;
    const bool opening = picker.purpose == FilePickerPurpose::OpenSurvey;
    const bool exporting = picker.purpose == FilePickerPurpose::Export;
    const bool image = picker.purpose == FilePickerPurpose::WaveformImage;
    const bool copy = picker.purpose == FilePickerPurpose::SaveCopy;
    const bool blocked = ui.operation_busy() || (snapshot.running && !image && !copy);
    ImGui::TextUnformatted(opening ? "Open saved session" : exporting ? "Export destination" :
        image ? "Save waveform image" : copy ? "Save session copy" : "New survey recording");
    ImGui::BeginDisabled(blocked);
    if (ImGui::Button("Home")) {
        try { browse_folder(picker, file_browser_home()); }
        catch (const std::exception& e) { picker.error = e.what(); }
    }
    ImGui::SameLine();
    if (ImGui::Button("Up")) {
        try { browse_folder(picker, file_browser_parent(picker.directory)); }
        catch (const std::exception& e) { picker.error = e.what(); }
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) browse_folder(picker, picker.directory);
    ImGui::SameLine();
    if (ImGui::BeginCombo("##localRoots", "Local disk", ImGuiComboFlags_WidthFitPreview)) {
        for (const auto& root : picker.roots) {
            if (ImGui::Selectable(root.c_str())) browse_folder(picker, root);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Show hidden", &picker.show_hidden)) filter_file_entries(picker);
    selected_file_field("##browseFolder", "Choose a local folder", picker.directory);
    ImGui::TextDisabled("Click a folder to enter it; click a file to select its name.");
    std::optional<std::string> next_directory;
    ImGui::BeginChild("fileEntries", {0, std::max(90.0f, ImGui::GetContentRegionAvail().y - 185)}, ImGuiChildFlags_Borders);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(picker.visible_entries.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const auto& entry = picker.listing.entries[picker.visible_entries[static_cast<size_t>(row)]];
            ImGui::PushID(row);
            // Filesystem names are untrusted text, never ImGui IDs or formats.
            const auto position = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##entry", !entry.directory && entry.name == picker.filename.data())) {
                if (entry.directory) {
                    try { next_directory = file_browser_join(picker.directory, entry.name); }
                    catch (const std::exception& e) { picker.error = e.what(); }
                } else if (entry.name.size() >= picker.filename.size()) {
                    picker.error = "This filename is too long for the chooser. Choose a shorter name.";
                } else {
                    copy_text(picker.filename, entry.name);
                    picker.error.clear();
                }
            }
            std::string display = (entry.directory ? "[Folder]  " : "          ") + entry.name;
            for (auto& c : display) if (static_cast<unsigned char>(c) < 32 || c == 127) c = '?';
            ImGui::GetWindowDrawList()->AddText(position, ImGui::GetColorU32(entry.directory ? secondary : ImGui::GetStyleColorVec4(ImGuiCol_Text)), display.c_str());
            ImGui::PopID();
        }
    }
    if (picker.visible_entries.empty()) ImGui::TextDisabled("No visible files or folders.");
    ImGui::EndChild();
    // Do not invalidate the list while its rows are being rendered.
    if (next_directory) browse_folder(picker, *next_directory);
    if (picker.listing.truncated) wrapped("This folder is too large to list completely. Use a smaller folder or enter the known filename below.", amber);
    if (exporting && ui.export_kind == 5) {
        ImGui::SetNextItemWidth(240);
        if (ImGui::Combo("File type", &picker.format, "CSV (.csv)\0GeoJSON (.geojson)\0")) {
            const std::string name = picker.filename.data();
            auto path = std::filesystem::path(std::u8string(name.begin(), name.end()));
            path.replace_extension(picker.format == 0 ? ".csv" : ".geojson");
            const auto updated = path_utf8(path);
            if (updated.size() >= picker.filename.size()) picker.error = "Filename is too long. Shorten it before choosing this format.";
            else copy_text(picker.filename, updated);
        }
    } else if (exporting) {
        picker.format = 0;
        ImGui::TextDisabled(ui.export_kind == 6 ? "File type: Analysis report (.html)" : "File type: CSV report (.csv)");
    } else ImGui::TextDisabled(image ? "File type: PNG image (.png)" : opening ? "Select an existing survey database." : "File type: Survey database (.sqlite)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##chosenFilename", "Filename", picker.filename.data(), picker.filename.size());
    if (!picker.error.empty()) wrapped(picker.error.c_str(), red);
    else if (ui.file_chosen) wrapped(opening ? "Open this saved survey read-only." : "Save a new file here. Existing files will not be replaced.");
    else wrapped(opening ? "Choosing a file does not open it until you press Open." : "The file will be created when recording or export starts. Existing recordings will not be replaced.");
    ImGui::BeginDisabled(picker.directory.empty() || picker.filename[0] == 0);
    if (ImGui::Button(ui.file_chosen ? (opening ? "Open" : "Save") : "Choose file", {145, 0})) {
        try {
            const auto kind = image ? FileChoiceKind::Png : exporting ? (ui.export_kind == 6 ? FileChoiceKind::Html :
                picker.format == 0 ? FileChoiceKind::Csv : FileChoiceKind::GeoJson) : FileChoiceKind::Survey;
            const auto path = choose_local_file(picker.directory, picker.filename.data(), kind, opening);
            if (ui.file_chosen) { auto complete = std::move(ui.file_chosen); ui.file_chosen = {}; complete(path); }
            else if (exporting) ui.export_path = path;
            else if (opening) ui.reopen_path = path;
            else {
                ui.session_path = path;
                ui.recording_path_used = false;
                if (ui.preferences_ready) {
                    ui.preferences.recording_directory = path_utf8(std::filesystem::path(std::u8string(path.begin(), path.end())).parent_path());
                    ui.persist_preferences();
                }
                ui.feedback(true, "Recording destination selected. The survey file will be created when reception starts.");
            }
            picker.purpose = FilePickerPurpose::None;
            ImGui::CloseCurrentPopup();
        } catch (const std::exception& e) { picker.error = e.what(); }
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {100, 0})) {
        picker.purpose = FilePickerPurpose::None;
        ui.file_chosen = {}; ui.capture_pixels.clear();
        ImGui::CloseCurrentPopup();
    }
    if (blocked) wrapped("Wait for the current operation, or stop reception before opening another survey.", amber);
    ImGui::EndPopup();
    if (!open) { picker.purpose = FilePickerPurpose::None; ui.file_chosen = {}; ui.capture_pixels.clear(); }
}

void metric(const char* name, const std::string& value, const char* detail, ImVec4 color = accent) {
    ImGui::TextColored(muted, "%s", name);
    ImGui::TextColored(color, "%s", value.c_str());
    if (detail) ImGui::TextDisabled("%s", detail);
}

void health_strip(const Snapshot& snapshot) {
    if (ImGui::BeginTable("health", 5, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        metric("SESSION TIME", duration_text(snapshot.elapsed_seconds), snapshot.historical ? "Saved session" : "Host elapsed time");
        ImGui::TableNextColumn();
        std::array<char, 64> number{};
        std::snprintf(number.data(), number.size(), "%.1f%%", ratio(snapshot.input_seconds, snapshot.elapsed_seconds) * 100);
        metric("INPUT AVAILABILITY", number.data(), "Delivered samples / elapsed");
        ImGui::TableNextColumn();
        std::snprintf(number.data(), number.size(), "%.1f%%", ratio(snapshot.measurement_seconds, snapshot.input_seconds) * 100);
        metric("MEASUREMENT DUTY", number.data(), "FFT windows / input time", secondary);
        ImGui::TableNextColumn();
        metric("ENERGY EVENTS", std::to_string(snapshot.spectrum_events), "Measured energy; not packets", secondary);
        ImGui::TableNextColumn();
        metric("CLIPPED SAMPLES", std::to_string(snapshot.clipped_samples), "ADC limit; distorted measurement", snapshot.clipped_samples ? amber : accent);
        ImGui::EndTable();
    }
}

void automatic_decoder_status(const Snapshot& snapshot, bool requested) {
    const auto& status = snapshot.automatic_decoder;
    if (snapshot.historical && !status.available) {
        wrapped("Automatic decoder diagnostics were not recorded in this survey.", secondary);
        return;
    }
    if (!status.enabled) {
        wrapped(requested && !snapshot.historical ? "Automatic decoding / ready for reception" : "Automatic decoding / off", requested ? accent : secondary);
        return;
    }
    ImGui::TextColored(accent, "Automatic decoding / %s / %zu active",
        snapshot.running ? "running" : snapshot.historical ? "recorded" : "stopped", status.active_decoders);
    ImGui::Text("Latest acquisition: %llu frames / %llu CRC valid / %llu likely Meshtastic",
        static_cast<unsigned long long>(status.completed), static_cast<unsigned long long>(status.crc_valid),
        static_cast<unsigned long long>(status.classified));
    const bool limited = status.history_misses || status.active_limit_hits || status.frame_overflows || status.abandoned_decoders;
    if (limited) ImGui::TextColored(amber, "Some candidates could not be processed / hover for counts");
    else ImGui::TextDisabled("Candidate processing is bounded; reception is not exhaustive.");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("Candidates: %llu / started: %llu / completed: %llu",
            static_cast<unsigned long long>(status.candidates), static_cast<unsigned long long>(status.started),
            static_cast<unsigned long long>(status.completed));
        ImGui::Text("History unavailable: %llu / active limit: %llu / frame overflow: %llu",
            static_cast<unsigned long long>(status.history_misses), static_cast<unsigned long long>(status.active_limit_hits),
            static_cast<unsigned long long>(status.frame_overflows));
        ImGui::Text("Unsupported: %llu / outside range: %llu / excluded: %llu / duplicate: %llu",
            static_cast<unsigned long long>(status.unsupported_candidates), static_cast<unsigned long long>(status.outside_range_candidates),
            static_cast<unsigned long long>(status.excluded_candidates), static_cast<unsigned long long>(status.duplicate_candidates));
        ImGui::Text("Timeouts: %llu / resets: %llu / abandoned: %llu",
            static_cast<unsigned long long>(status.timeouts), static_cast<unsigned long long>(status.resets),
            static_cast<unsigned long long>(status.abandoned_decoders));
        ImGui::TextUnformatted("Counters describe processing stages, not unique packets or measured airtime.");
        ImGui::EndTooltip();
    }
}

bool rak_preset_supported(const meshtastic::Preset& preset) {
    return (preset.bandwidth_hz == 125000 || preset.bandwidth_hz == 250000 || preset.bandwidth_hz == 500000) &&
        preset.spreading_factor >= 7 && preset.spreading_factor <= 12;
}

void meshtastic_preset_catalog(bool rak = false) {
    if (!ImGui::CollapsingHeader("Meshtastic preset catalog", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::Text("%zu modem presets / one shared public default key", meshtastic::presets.size());
    wrapped("One public default key (AQ==) applies across presets. Presets define modulation, not a channel key or regional frequency.");
    if (ImGui::BeginTable("meshtasticPresets", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, {0, 255})) {
        ImGui::TableSetupColumn("Preset", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("BW / kHz", ImGuiTableColumnFlags_WidthStretch, .8f);
        ImGui::TableSetupColumn("SF", ImGuiTableColumnFlags_WidthStretch, .5f);
        ImGui::TableSetupColumn("CR", ImGuiTableColumnFlags_WidthStretch, .6f);
        ImGui::TableSetupColumn(rak ? "RAK support" : "SDR support", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
        for (const auto& preset : meshtastic::presets) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(preset.name.data(), preset.name.data() + preset.name.size());
            ImGui::TableNextColumn(); ImGui::Text("%.3f", preset.bandwidth_hz / 1000.0);
            ImGui::TableNextColumn(); ImGui::Text("%u", static_cast<unsigned>(preset.spreading_factor));
            ImGui::TableNextColumn(); ImGui::Text("4/%u", static_cast<unsigned>(preset.coding_rate_denominator));
            ImGui::TableNextColumn();
            const bool supported = rak ? rak_preset_supported(preset) : preset.phy_supported;
            ImGui::TextColored(supported ? accent : secondary, "%s", supported ? "Available" : "Unsupported");
            if (ImGui::IsItemHovered() && (rak || !preset.support_note.empty())) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
                if (rak) ImGui::TextUnformatted("RAK supports configured 125/250/500 kHz, SF7-12 reception. Narrower modes require an SDR decoder that supports that bandwidth. This does not select a regional frequency.");
                else ImGui::TextUnformatted(preset.support_note.data(), preset.support_note.data() + preset.support_note.size());
                ImGui::PopTextWrapPos(); ImGui::EndTooltip();
            }
            if (preset.deprecated) ImGui::TextColored(amber, "Deprecated");
            if (preset.historical_only) ImGui::TextDisabled("Historical");
            else if (!preset.available_in_2_7_19) ImGui::TextDisabled("2.8+");
        }
        ImGui::EndTable();
    }
    wrapped("Available means the modulation can be attempted, not guaranteed reception. Deprecated presets remain listed for existing traffic. Unsupported presets remain visible in RF measurements.", secondary);
}

void meshtastic_preset_picker(LaneConfig& lane) {
    const meshtastic::Preset* match = nullptr;
    for (const auto& preset : meshtastic::presets)
        if (preset.bandwidth_hz == lane.bandwidth_hz && preset.spreading_factor == lane.spreading_factor &&
            preset.coding_rate_denominator == lane.coding_rate) { match = &preset; break; }
    const std::string preview = match ? std::string(match->name) : "Custom modulation";
    if (ImGui::BeginCombo("Meshtastic preset", preview.c_str())) {
        for (const auto& preset : meshtastic::presets) {
            const auto name = std::string(preset.name) + (preset.historical_only ? " (historical)" : preset.deprecated ? " (deprecated)" : "") +
                (!preset.phy_supported ? " (unsupported)" : "");
            ImGui::BeginDisabled(!preset.phy_supported);
            if (ImGui::Selectable(name.c_str(), match == &preset)) {
                lane.label = std::string(preset.name); lane.bandwidth_hz = preset.bandwidth_hz;
                lane.spreading_factor = preset.spreading_factor; lane.coding_rate = preset.coding_rate_denominator;
            }
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    wrapped("Choosing a preset changes BW/SF/CR only. Check the frequency below for your region; keys stay survey-wide.", secondary);
}

void legacy_profile_settings(Engine& engine, DesktopState& ui, const Snapshot& snapshot) {
    label("LEGACY DECODE PROFILES");
    wrapped("Optional fixed-frequency receivers supplement automatic discovery. No manual profile is required for automatic decoding.");
    ImGui::BeginDisabled(snapshot.running || snapshot.historical || ui.operation_busy());
    for (size_t i = 0; i < ui.config.lanes.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        auto& lane = ui.config.lanes[i];
        ImGui::Checkbox("##enabled", &lane.enabled);
        ImGui::SameLine();
        const std::string title = lane.label + " / SF" + std::to_string(lane.spreading_factor);
        if (ImGui::Selectable(title.c_str(), ui.selected_lane == static_cast<int>(i))) {
            ui.selected_lane = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (!ui.config.lanes.empty()) {
        ui.selected_lane = std::clamp(ui.selected_lane, 0, static_cast<int>(ui.config.lanes.size()) - 1);
        auto& lane = ui.config.lanes[static_cast<size_t>(ui.selected_lane)];
        meshtastic_preset_picker(lane);
        double frequency = static_cast<double>(lane.frequency_hz) / 1e6;
        ImGui::TextUnformatted("Selected profile / MHz");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputDouble("##laneFrequency", &frequency, 0.025, 0.25, "%.6f") && std::isfinite(frequency))
            lane.frequency_hz = static_cast<uint64_t>(std::clamp(frequency, 1.0, 6000.0) * 1e6);
        const char* bandwidths[] = {"15.625 kHz", "62.5 kHz", "125 kHz", "250 kHz", "500 kHz"};
        const std::array<uint32_t, 5> bandwidth_values{15625, 62500, 125000, 250000, 500000};
        int bandwidth = 1;
        for (size_t i = 0; i < bandwidth_values.size(); ++i) if (lane.bandwidth_hz == bandwidth_values[i]) bandwidth = static_cast<int>(i);
        if (ImGui::Combo("BW", &bandwidth, bandwidths, 5)) lane.bandwidth_hz = bandwidth_values[static_cast<size_t>(bandwidth)];
        int sf = lane.spreading_factor;
        int cr = lane.coding_rate;
        if (ImGui::SliderInt("SF", &sf, 7, 12)) lane.spreading_factor = static_cast<uint8_t>(sf);
        if (ImGui::SliderInt("CR", &cr, 5, 8, "4/%d")) lane.coding_rate = static_cast<uint8_t>(cr);
        help("Bandwidth, spreading factor, and coding rate are configured expectations. A decode profile observes one combination; RF survey coverage does not mean all possible LoRa profiles are being decoded.");
    }
    if (ui.config.lanes.size() < 4 && ImGui::Button("Add profile", {-1, 0})) {
        LaneConfig lane;
        lane.label = "Profile " + std::to_string(ui.config.lanes.size() + 1);
        lane.frequency_hz = ui.config.center_hz;
        ui.config.lanes.push_back(lane);
        ui.selected_lane = static_cast<int>(ui.config.lanes.size()) - 1;
    }
    if (!ui.config.lanes.empty() && ImGui::Button("Remove selected profile", {-1, 0})) {
        ui.config.lanes.erase(ui.config.lanes.begin() + ui.selected_lane);
        ui.selected_lane = 0;
        ui.feedback(true, "Receive profile removed. Authorized key records remain independent of receiver profiles.");
    }
    ImGui::EndDisabled();
    if (ImGui::Button("Authorized channel keys", {-1, 0})) {
        const auto records = engine.key_records();
        const auto& record = records[static_cast<size_t>(ui.selected_key_record)];
        if (record.configured) {
            ui.key_mode = record.restrict_channel_name ? 0 : 1;
            if (!record.channel_name.empty()) copy_text(ui.channel_name, record.channel_name);
            copy_text(ui.key_label, record.label);
        }
        erase_secret(ui.key_input);
        ui.authorize_keys = false;
        ui.show_keys = true;
    }
    wrapped("MeshCore and recipient private-key decoding are not yet enabled. Message contents are not interpreted.");
}

void plot_grabber(const char* id, ImVec2 origin, float width, float& size,
                  float minimum, float maximum, float scale, const char* guidance = nullptr) {
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton(id, {width, 12 * scale});
    const bool hover = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    if (hover || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0))
        size = std::clamp(size + ImGui::GetIO().MouseDelta.y / scale, minimum, maximum);
    // Keyboard users can adjust the focused divider without precise dragging.
    if (ImGui::IsItemFocused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) size = std::max(minimum, size - 10);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) size = std::min(maximum, size + 10);
    }
    auto* draw = ImGui::GetWindowDrawList();
    const float y = origin.y + 6 * scale, middle = origin.x + width * .5f;
    const ImU32 color = ImGui::GetColorU32(hover || active ? accent : muted);
    draw->AddLine({origin.x, y}, {origin.x + width, y}, IM_COL32(34, 48, 62, 255));
    draw->AddLine({middle - 22 * scale, y - scale}, {middle + 22 * scale, y - scale}, color, 2 * scale);
    draw->AddLine({middle - 22 * scale, y + 2 * scale}, {middle + 22 * scale, y + 2 * scale}, color, scale);
    if (hover) ImGui::SetTooltip("%s\nFocus and use arrow keys for smaller adjustments.", guidance ? guidance : "Drag up or down to resize.");
}

void spectrum_view(DesktopState& ui, const Snapshot& snapshot, float height) {
    ImGui::TextUnformatted("Spectrum & waterfall");
    ImGui::SameLine();
    ImGui::TextDisabled("Uncalibrated dBFS");
    ImGui::SameLine();
    ImGui::Checkbox("Freeze display", &ui.freeze_waterfall);
    help("Freezing the display does not pause reception or survey measurements. The waterfall is a transient visual history, not a saved recording.");
    ImGui::SameLine();
    const bool overlay = ui.show_settings || ui.show_keys || ui.show_detail || ui.show_export || ui.show_licenses ||
        ui.show_diagnostics || ui.show_analysis_details || ui.new_requested || ui.close_requested ||
        ui.file_picker.purpose != FilePickerPurpose::None;
    ImGui::BeginDisabled(overlay || snapshot.spectrum_dbfs.empty() || ui.capture_requested || ui.operation_busy());
    if (ImGui::Button("Capture PNG...")) ui.capture_requested = true;
    ImGui::EndDisabled();
    // Stopped plots retain the acquisition axis, even while next-session
    // receiver settings are edited in the sidebar.
    const auto& cfg = snapshot.session_id.empty() ? ui.config : snapshot.config;
    if (snapshot.session_id != ui.last_session || cfg.center_hz != ui.waterfall_center_hz || cfg.survey_span_hz != ui.waterfall_span_hz) {
        ui.waterfall_center_hz = cfg.center_hz; ui.waterfall_span_hz = cfg.survey_span_hz;
        ui.last_session = snapshot.session_id;
        ui.waterfall.clear();
        ui.last_spectrum = 0;
    }
    if (!ui.freeze_waterfall && snapshot.spectrum_sequence != ui.last_spectrum && !snapshot.spectrum_dbfs.empty()) {
        ui.waterfall.push_front(snapshot.spectrum_dbfs);
        if (ui.waterfall.size() > waterfall_rows) ui.waterfall.pop_back();
        ui.last_spectrum = snapshot.spectrum_sequence;
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(200.0f, ImGui::GetContentRegionAvail().x);
    const float scale = ui.ui_scale;
    // Leave a useful result table even at the minimum window size. A smaller
    // window clamps the visible sizes; it cannot drag either panel out of reach.
    const float plot_budget = std::max(130.f, height / scale - 83.f);
    ui.spectrum_height = std::clamp(ui.spectrum_height, 60.f, plot_budget - 70.f);
    ui.waterfall_height = std::clamp(ui.waterfall_height, 70.f, plot_budget - ui.spectrum_height);
    height = (ui.spectrum_height + ui.waterfall_height + 83.f) * scale;
    ui.capture_origin = origin; ui.capture_size = {width, height};
    ImGui::Dummy({width, height});
    const ImVec2 after_plot = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    const float left = origin.x + 48;
    const float right = origin.x + width - 12;
    const float top = origin.y + 10;
    const float spectrum_bottom = top + ui.spectrum_height * scale;
    ui.spectrum_grabber = {origin.x, spectrum_bottom + 27 * scale};
    const float waterfall_top = ui.spectrum_grabber.y + 12 * scale;
    const float bottom = waterfall_top + ui.waterfall_height * scale;
    ui.waterfall_grabber = {origin.x, bottom + 22 * scale};
    const float plot_width = right - left;
    const double start_hz = static_cast<double>(cfg.center_hz) - cfg.survey_span_hz * 0.5;
    const double end_hz = start_hz + cfg.survey_span_hz;
    draw->AddRectFilled(origin, {origin.x + width, origin.y + height}, IM_COL32(6, 12, 20, 255), 5);
    draw->PushClipRect({left, top}, {right, bottom}, true);
    for (int grid = 0; grid <= 4; ++grid) {
        const float y = top + (spectrum_bottom - top) * static_cast<float>(grid) / 4;
        draw->AddLine({left, y}, {right, y}, IM_COL32(34, 48, 62, 255));
    }
    for (int grid = 0; grid <= 8; ++grid) {
        const float x = left + plot_width * static_cast<float>(grid) / 8;
        draw->AddLine({x, top}, {x, bottom}, IM_COL32(30, 43, 55, 255));
    }
    if (!snapshot.spectrum_dbfs.empty()) {
        std::vector<ImVec2> points;
        points.reserve(snapshot.spectrum_dbfs.size());
        for (size_t i = 0; i < snapshot.spectrum_dbfs.size(); ++i) {
            const float level = std::clamp((snapshot.spectrum_dbfs[i] - ui.display_floor) /
                                          std::max(1.0f, ui.display_ceiling - ui.display_floor), 0.0f, 1.0f);
            points.emplace_back(left + plot_width * (static_cast<float>(i) + 0.5f) / static_cast<float>(snapshot.spectrum_dbfs.size()),
                                spectrum_bottom - level * (spectrum_bottom - top));
        }
        draw->AddPolyline(points.data(), static_cast<int>(points.size()), IM_COL32(80, 215, 201, 255), 0, 1.2f);
        // Resizing reveals or clips history; it must never stretch the same
        // signal vertically to fill a larger panel. UI scale is independent.
        const float row_height = waterfall_row_pitch * scale;
        for (size_t row = 0; row < ui.waterfall.size(); ++row) {
            const auto& samples = ui.waterfall[row];
            const float y = waterfall_top + static_cast<float>(row) * row_height;
            if (y >= bottom) break;
            const size_t columns = std::min(samples.size(), static_cast<size_t>(std::max(1.0f, plot_width / 2)));
            for (size_t column = 0; column < columns; ++column) {
                const size_t begin = column * samples.size() / columns;
                const size_t end = (column + 1) * samples.size() / columns;
                float strongest = ui.display_floor;
                for (size_t i = begin; i < end; ++i) strongest = std::max(strongest, samples[i]);
                const float x = left + plot_width * static_cast<float>(column) / static_cast<float>(columns);
                draw->AddRectFilled({x, y}, {x + plot_width / static_cast<float>(columns) + 0.5f, std::min(bottom, y + row_height + 0.5f)},
                                    heat_color(strongest, ui.display_floor, ui.display_ceiling));
            }
        }
    }
    const double span = std::max(1.0, end_hz - start_hz);
    for (size_t i = 0; i < cfg.lanes.size(); ++i) {
        const auto& lane = cfg.lanes[i];
        if (!lane.enabled) continue;
        const float x = left + static_cast<float>((static_cast<double>(lane.frequency_hz) - start_hz) / span) * plot_width;
        const float half_width = static_cast<float>(lane.bandwidth_hz / span) * plot_width * 0.5f;
        if (x + half_width < left || x - half_width > right) continue;
        const bool selected = static_cast<int>(i) == ui.selected_lane;
        draw->AddRectFilled({x - half_width, top}, {x + half_width, spectrum_bottom}, selected ? IM_COL32(80, 180, 170, 27) : IM_COL32(110, 160, 220, 20));
        draw->AddLine({x, top}, {x, bottom}, selected ? IM_COL32(109, 237, 208, 190) : IM_COL32(126, 179, 236, 150));
        draw->AddText({x + 4, top + 4 + static_cast<float>(i % 3) * 16}, selected ? IM_COL32(130, 245, 217, 255) : IM_COL32(149, 190, 231, 255), lane.label.c_str());
    }
    draw->PopClipRect();
    for (int grid = 0; grid <= 4; ++grid) {
        std::array<char, 24> text{};
        const float level = ui.display_ceiling - (ui.display_ceiling - ui.display_floor) * static_cast<float>(grid) / 4;
        std::snprintf(text.data(), text.size(), "%.0f", level);
        draw->AddText({origin.x + 8, top + (spectrum_bottom - top) * static_cast<float>(grid) / 4 - 6}, IM_COL32(132, 152, 171, 255), text.data());
    }
    const int ticks = plot_width / 8 >= ImGui::CalcTextSize("9999.999").x + 14 ? 8 : 4;
    for (int grid = 0; grid <= ticks; ++grid) {
        std::array<char, 32> text{};
        std::snprintf(text.data(), text.size(), "%.3f", (start_hz + span * grid / ticks) / 1e6);
        const float x = left + plot_width * static_cast<float>(grid) / static_cast<float>(ticks);
        const ImVec2 text_size = ImGui::CalcTextSize(text.data());
        draw->AddText({std::clamp(x - text_size.x / 2, left, right - text_size.x), spectrum_bottom + 7}, IM_COL32(155, 173, 192, 255), text.data());
    }
    std::array<char, 160> context{};
    const double utc = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::snprintf(context.data(),context.size(),"%s | MHz / relative dBFS | %.1f MS/s | View %s%s",
        receiver_source_name(cfg),cfg.sample_rate/1e6,
        clock_text(utc).c_str(),ui.freeze_waterfall?" / frozen display":snapshot.running?"":" / stopped results");
    draw->PushClipRect(origin,{origin.x+width,origin.y+height},true);
    draw->AddText({origin.x+10,bottom+8},ImGui::GetColorU32(muted),context.data());
    draw->PopClipRect();
    if (snapshot.spectrum_dbfs.empty()) {
        const char* message = snapshot.historical ? "Live spectrum is not stored in saved sessions" : "Start a session to receive spectrum samples";
        const ImVec2 size = ImGui::CalcTextSize(message);
        draw->AddText({left + (plot_width - size.x) / 2, waterfall_top + 28}, IM_COL32(128, 151, 171, 255), message);
    }
    bool plot_hovered = false, plot_clicked = false;
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##spectrum", {width, ui.spectrum_grabber.y - origin.y});
    plot_hovered = ImGui::IsItemHovered(); plot_clicked = ImGui::IsItemClicked();
    ImGui::SetCursorScreenPos({origin.x, waterfall_top});
    ImGui::InvisibleButton("##waterfall", {width, bottom - waterfall_top});
    plot_hovered |= ImGui::IsItemHovered(); plot_clicked |= ImGui::IsItemClicked();
    if (plot_hovered) {
        const float x = std::clamp(ImGui::GetIO().MousePos.x, left, right);
        const double frequency = start_hz + (x - left) / plot_width * span;
        ImGui::SetTooltip("%.6f MHz\n%s", frequency / 1e6,
            snapshot.running ? "Stop reception before changing the selected profile." :
            snapshot.historical ? "Historical survey. Start a new session to tune." :
            "Click to tune the selected decode profile. Survey center is unchanged.");
        if (plot_clicked && !snapshot.running && !snapshot.historical && !ui.config.lanes.empty()) {
            auto& lane = ui.config.lanes[static_cast<size_t>(ui.selected_lane)];
            if (cfg.survey_span_hz >= lane.bandwidth_hz)
                lane.frequency_hz = static_cast<uint64_t>(std::clamp(std::round(frequency / 1000) * 1000,
                    start_hz + lane.bandwidth_hz / 2.0, end_hz - lane.bandwidth_hz / 2.0));
        }
    }
    plot_grabber("##spectrumDivider", ui.spectrum_grabber, width, ui.spectrum_height,
        60.f, plot_budget - ui.waterfall_height, scale);
    plot_grabber("##waterfallDivider", ui.waterfall_grabber, width, ui.waterfall_height,
        70.f, plot_budget - ui.spectrum_height, scale,
        "Drag to reveal more or fewer history rows without stretching signals.\nRows are display updates, not an exact elapsed-time scale.");
    ImGui::SetCursorScreenPos(after_plot);

}

void reception_detail(const Reception& reception) {
    ImGui::TextColored(accent, "%s", reception.decoded.classification.c_str());
    ImGui::Text("Classification state: %.*s", static_cast<int>(protocol::status_name(reception.decoded.status).size()), protocol::status_name(reception.decoded.status).data());
    const auto explanation = protocol::status_explanation(reception.decoded.status);
    ImGui::TextWrapped("%.*s", static_cast<int>(explanation.size()), explanation.data());
    wrapped(reception.decoded.authentication.c_str(), amber);
    ImGui::Separator();
    ImGui::Text("%s UTC / host-estimated reception time", timestamp_text(reception.utc_seconds).c_str());
    ImGui::Text("Elapsed session time: %.6f s", reception.elapsed_seconds);
    ImGui::Text("%.6f MHz  |  %.1f kHz  |  SF%u  |  CR 4/%u", static_cast<double>(reception.frequency_hz) / 1e6,
                static_cast<double>(reception.bandwidth_hz) / 1e3, reception.spreading_factor, reception.coding_rate);
    ImGui::Text("PHY header %s  |  PHY CRC %s", reception.header_valid ? "valid" : "invalid / unavailable", reception.crc_valid ? "valid" : "invalid");
    if (reception.concentrator) {
        const auto& meta = *reception.concentrator;
        ImGui::Text("Board %u | RSSI %.1f dBm (uncalibrated) | SNR %.1f dB", meta.board_index+1,meta.rssi_dbm,reception.snr_db);
        ImGui::Text("Board-local timestamp: %u us (wrapping counter)",meta.hardware_timestamp_us);
        ImGui::TextDisabled("Frequency and bandwidth are configured modem values, not independent signal-width measurements.");
        ImGui::TextDisabled("This receiver does not measure packet duration or frequency error here.");
    } else ImGui::Text("Duration %.3f s  |  Estimated SNR %.1f dB  |  Frequency error %.0f Hz",
                reception.duration_seconds, reception.snr_db, reception.frequency_error_hz);
    ImGui::TextDisabled("PHY checks establish reception integrity, not sender identity.");
    if (reception.receiver_position && reception.receiver_position->valid) {
        const auto& fix = *reception.receiver_position;
        ImGui::Text("Receiver position: %.6f, %.6f (%s)", fix.latitude, fix.longitude, fix.manual ? "fixed / manual" : fix.source.c_str());
    } else ImGui::TextDisabled("Receiver position unavailable or stale for this reception.");
    if (reception.decoded.evidence) {
        const auto& evidence=*reception.decoded.evidence;
        ImGui::Text("Envelope evidence: port %u | signature %s", evidence.port,
            evidence.signature_present ? "present, not verified" : "not present");
        ImGui::TextDisabled("Envelope evidence does not authenticate a sender or identify the physical transmitter.");
    }
    ImGui::Spacing();
    wrapped("Likely protocol classification is an unauthenticated envelope inference and may be a false positive. Message contents and sender identities are not interpreted or retained.");
}

bool matches_filter(const Reception& reception, const DesktopState& ui) {
    if (ui.only_classified && reception.decoded.status != protocol::Status::classified) return false;
    if (ui.filter[0] == 0) return true;
    std::string needle = ui.filter.data();
    std::string haystack = reception.lane_label + " " + reception.decoded.classification;
    const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c); };
    std::transform(needle.begin(), needle.end(), needle.begin(), lower);
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), lower);
    return haystack.find(needle) != std::string::npos;
}

void packet_table(DesktopState& ui, const Snapshot& snapshot, float height) {
    ImGui::TextUnformatted("RECEPTION LOG");
    ImGui::SameLine();
    ImGui::TextDisabled("%zu recent records", snapshot.receptions.size());
    ImGui::SameLine();
    ImGui::Checkbox("Likely Meshtastic", &ui.only_classified);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(110.0f, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##filter", "Filter profile or classification...", ui.filter.data(), ui.filter.size());
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("receptions", 7, flags, {0, std::max(90.0f, height)})) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Received at (UTC)", ImGuiTableColumnFlags_WidthFixed,
            ImGui::CalcTextSize("0000-00-00 00:00:00.000").x + 12);
        ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("BW / SF", ImGuiTableColumnFlags_WidthFixed, 87);
        ImGui::TableSetupColumn("Classification", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Classification state", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("Envelope evidence", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("GPS", ImGuiTableColumnFlags_WidthFixed, 46);
        ImGui::TableHeadersRow();
        for (const auto& reception : snapshot.receptions) {
            if (!matches_filter(reception, ui)) continue;
            ImGui::PushID(static_cast<int>(reception.id & 0x7fffffff));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool selected = ui.selected && ui.selected->id == reception.id;
            if (ImGui::Selectable(timestamp_text(reception.utc_seconds).c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                ui.selected = reception;
                ui.show_detail = true;
            }
            reception_time_tooltip(reception.utc_seconds, reception.elapsed_seconds, false);
            ImGui::TableNextColumn();
            ImGui::Text("%.6f", static_cast<double>(reception.frequency_hz) / 1e6);
            ImGui::TableNextColumn();
            ImGui::Text("%.0fk / %u", static_cast<double>(reception.bandwidth_hz) / 1000, reception.spreading_factor);
            ImGui::TableNextColumn();
            ImGui::TextColored(reception.decoded.status == protocol::Status::classified ? accent : amber, "%s", reception.decoded.classification.c_str());
            ImGui::TableNextColumn();
            const auto decode_status = protocol::status_name(reception.decoded.status);
            ImGui::TextUnformatted(decode_status.data(), decode_status.data() + decode_status.size());
            if (ImGui::IsItemHovered()) {
                const auto explanation = protocol::status_explanation(reception.decoded.status);
                ImGui::SetTooltip("%.*s", static_cast<int>(explanation.size()), explanation.data());
            }
            ImGui::TableNextColumn();
            if (reception.decoded.evidence) {
                ImGui::Text("Port %u | %s", reception.decoded.evidence->port,
                    reception.decoded.evidence->signature_present ? "signature unverified" : "no signature");
            } else ImGui::TextDisabled("Unavailable");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(reception.receiver_position && reception.receiver_position->valid ? "fix" : "--");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (snapshot.receptions.empty()) wrapped(is_concentrator(snapshot.config)
        ? "No frames received yet. Sampled RF energy has separate measurements; packets require a matching configured modem profile."
        : "No frames received yet. Unknown RF activity remains visible in the spectrum and occupancy measurements.");
}

void position_view_controls(DesktopState& ui) {
    int choice = ui.position_view_mode == PositionViewMode::Mobile ? 1 : 0;
    ImGui::SetNextItemWidth(std::min(300.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("Position display", &choice, "Stationary / position cloud\0Mobile / connected fixes\0"))
        ui.position_view_mode = choice ? PositionViewMode::Mobile : PositionViewMode::Stationary;
    wrapped(ui.position_view_mode == PositionViewMode::Stationary ?
        "Stationary view: dots show reported position variation; the white cross is their mean, not a surveyed location. No movement is inferred." :
        "Mobile view: lines connect successive position estimates. GPS variation may still look like movement; gaps break the line.");
}

void position_plot_guides(const GeographicPlot& plot, ImVec2 origin, ImVec2 size) {
    auto* draw = ImGui::GetWindowDrawList();
    const float step = static_cast<float>(plot.scale_bar_metres / plot.metres_per_pixel);
    if (step > 0 && std::isfinite(step)) {
        // A square grid uses the same physical distance on both axes.
        for (float x = size.x / 2; x <= size.x; x += step) {
            draw->AddLine({origin.x + x, origin.y}, {origin.x + x, origin.y + size.y}, IM_COL32(31, 47, 60, 255));
            if (x != size.x / 2) draw->AddLine({origin.x + size.x - x, origin.y}, {origin.x + size.x - x, origin.y + size.y}, IM_COL32(31, 47, 60, 255));
        }
        for (float y = size.y / 2; y <= size.y; y += step) {
            draw->AddLine({origin.x, origin.y + y}, {origin.x + size.x, origin.y + y}, IM_COL32(31, 47, 60, 255));
            if (y != size.y / 2) draw->AddLine({origin.x, origin.y + size.y - y}, {origin.x + size.x, origin.y + size.y - y}, IM_COL32(31, 47, 60, 255));
        }
    }
    draw->AddText({origin.x + size.x - 12, origin.y + 4}, IM_COL32(140, 158, 173, 255), "N");
    const ImVec2 bar{origin.x, origin.y + size.y + 12};
    constexpr ImU32 ink = IM_COL32(200, 212, 223, 255);
    draw->AddLine(bar, {bar.x + step, bar.y}, ink, 2);
    draw->AddLine({bar.x, bar.y - 4}, {bar.x, bar.y + 4}, ink, 2);
    draw->AddLine({bar.x + step, bar.y - 4}, {bar.x + step, bar.y + 4}, ink, 2);
    char distance[80];
    std::snprintf(distance, sizeof(distance), "%g m / %.0f ft", plot.scale_bar_metres, plot.scale_bar_metres / .3048);
    draw->AddText({bar.x, bar.y + 7}, ink, distance);
}

ImVec2 position_plot_point(const GeographicPoint& point, ImVec2 origin) {
    return {origin.x + static_cast<float>(point.x), origin.y + static_cast<float>(point.y)};
}

void position_mean_marker(const GeographicPlot& plot, ImVec2 origin) {
    const auto mean = plot.mean_point();
    if (!mean) return;
    const auto p = position_plot_point(*mean, origin);
    constexpr ImU32 ink = IM_COL32(239, 250, 243, 255);
    ImGui::GetWindowDrawList()->AddLine({p.x - 7, p.y}, {p.x + 7, p.y}, ink, 2);
    ImGui::GetWindowDrawList()->AddLine({p.x, p.y - 7}, {p.x, p.y + 7}, ink, 2);
}

void position_spread_text(const GeographicPlot& plot) {
    ImGui::TextWrapped("%zu displayed fixes | Reported spread: %.1f m east-west, %.1f m north-south",
        plot.count, plot.data_east_west_metres, plot.data_north_south_metres);
    wrapped("North up; approximate local distances. Equal distance scale on both axes; minimum view 100 m across its shorter side. Spread is not an accuracy radius. Original fixes are unchanged.");
}

void position_quality_tooltip(const PositionFix& fix) {
    ImGui::Text("Receiver %.6f, %.6f (%s)", fix.latitude, fix.longitude, fix.manual ? "manual" : "GPS");
    if (fix.hdop) ImGui::Text("HDOP: %.2f (geometry, not metres of accuracy)", *fix.hdop);
    else ImGui::TextUnformatted("HDOP: unavailable");
    if (fix.satellites) ImGui::Text("Satellites: %u", fix.satellites);
    else ImGui::TextUnformatted("Satellites: unavailable");
    ImGui::TextUnformatted("A valid fix does not establish position accuracy.");
}

void track_view(const Snapshot& snapshot, DesktopState& ui, float height) {
    label("RECEIVER POSITIONS / OFFLINE VIEW");
    ImGui::PushID("receiverPositions");
    position_view_controls(ui);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size{std::max(200.0f, ImGui::GetContentRegionAvail().x), std::max(160.0f, height)};
    ImGui::InvisibleButton("##track", size);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(7, 15, 23, 255), 5);
    const ImVec2 plot_origin{origin.x + 20, origin.y + 12};
    const ImVec2 plot_size{size.x - 40, size.y - 58};
    std::vector<const PositionFix*> fixes;
    for (const auto& fix : snapshot.track) fixes.push_back(&fix);
    const GeographicPlot plot(fixes, plot_size.x, plot_size.y);
    if (!plot.drawable) {
        draw->AddText({origin.x + 18, origin.y + 26}, IM_COL32(139, 163, 181, 255), "No valid receiver positions in this session.");
        ImGui::PopID();
        return;
    }
    position_plot_guides(plot, plot_origin, plot_size);
    const PositionFix* previous = nullptr;
    std::optional<ImVec2> previous_point;
    const PositionFix* hovered = nullptr;
    float closest = 100;
    const auto mouse = ImGui::GetIO().MousePos;
    for (const auto& fix : snapshot.track) {
        const auto projected = plot.point(fix);
        if (!projected) { previous = nullptr; previous_point.reset(); continue; }
        const auto point = position_plot_point(*projected, plot_origin);
        if (ui.position_view_mode == PositionViewMode::Mobile && previous) {
            const double gap = fix.monotonic_seconds - previous->monotonic_seconds;
            if (std::isfinite(gap) && gap >= 0 && gap <= 30 && fix.manual == previous->manual)
                draw->AddLine(*previous_point, point, IM_COL32(57, 169, 170, 210), 2);
        }
        draw->AddCircleFilled(point, 2.5f, fix.manual ? IM_COL32(245, 188, 103, 255) : IM_COL32(104, 229, 200, 255));
        const float distance = (point.x - mouse.x) * (point.x - mouse.x) + (point.y - mouse.y) * (point.y - mouse.y);
        if (distance < closest) { closest = distance; hovered = &fix; }
        previous = &fix; previous_point = point;
    }
    if (ui.position_view_mode == PositionViewMode::Stationary) position_mean_marker(plot, plot_origin);
    if (ImGui::IsItemHovered() && hovered) {
        ImGui::BeginTooltip(); position_quality_tooltip(*hovered); ImGui::EndTooltip();
    }
    position_spread_text(plot);
    ImGui::PopID();
}

void lane_health_table(const Snapshot& snapshot) {
    label("DECODER COVERAGE");
    if (!is_concentrator(snapshot.config)) automatic_decoder_status(snapshot, snapshot.config.automatic_decode);
    wrapped("Manual profiles cover their configured frequencies and modulation. Automatic decoding follows discovered waveforms separately. Counts do not establish exhaustive traffic capture or end-to-end delivery.");
    if (ImGui::BeginTable("laneHealth", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        for (const char* header : {"Profile", "MHz", "Processed", "Frames", "Classified", "CRC fail", "State"}) ImGui::TableSetupColumn(header);
        ImGui::TableHeadersRow();
        for (const auto& lane : snapshot.lane_health) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(lane.label.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.6f", static_cast<double>(lane.frequency_hz) / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.1f s", lane.processed_seconds);
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(lane.frames));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(lane.classified));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(lane.crc_failures));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(lane.state.c_str());
        }
        ImGui::EndTable();
    }
}

void recording_mode_control(DesktopState& ui, const Snapshot& snapshot) {
    ImGui::BeginDisabled(snapshot.running);
    int mode = ui.config.compact_recording ? 0 : 1;
    if (ImGui::Combo("Recording mode", &mode, "Compact (default)\0Detailed (20 ms powers)\0")) {
        ui.config.compact_recording = mode == 0;
        ui.persist_preferences();
    }
    ImGui::EndDisabled();
    wrapped(ui.config.compact_recording
        ? "Compact: saves roughly one-second power summaries while retaining fine activity timing and original GPS fixes."
        : "Detailed: retains approximately 20 ms power measurements and uses more storage.");
    wrapped("This setting applies to new recordings only. Existing surveys and the original GPS precision remain unchanged.");
}

void recording_settings(DesktopState& ui, const Snapshot& snapshot) {
    label("SESSION & RECORDING");
    ImGui::BeginDisabled(snapshot.running || !snapshot.session_id.empty());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("Title", ui.session_title.data(), ui.session_title.size());
    if (ImGui::Checkbox("Save survey measurements", &ui.save_session)) {
        ui.persist_preferences();
        if (ui.save_session && (ui.session_path.empty() || ui.recording_path_used)) ui.prepare_recording_file();
    }
    recording_mode_control(ui, snapshot);
    if (ui.save_session) {
        if (ImGui::Button("Browse recording location...")) begin_file_picker(ui, FilePickerPurpose::SaveSurvey, ui.session_path);
        selected_file_field("##sessionPath", "No recording file selected", ui.session_path);
        if (ui.preferences_active) wrapped("The folder is remembered. Every new survey gets a separate file; previous recordings are never replaced.");
        wrapped("Choose a local data directory outside this source repository. Saved measurements may include precise receiver locations and private survey notes.", amber);
    } else wrapped("Memory-only session. Enable saving before starting to retain or export a survey; current memory-only data is discarded when replaced or closed.");
    ImGui::EndDisabled();
    if (!snapshot.session_id.empty()) wrapped("This survey keeps its recording file and mode when resumed. Use New to choose a different recording setup.");
    if (!ui.preferences_error.empty()) wrapped(ui.preferences_error.c_str(), red);
    ImGui::Text("Recording: %s", snapshot.recording ? "active" : snapshot.historical ? "saved session open" : "off");
    if (snapshot.recording) ImGui::TextWrapped("Current file: %s", snapshot.config.session_path.c_str());
    if (!snapshot.session_id.empty()) ImGui::Text("Session ID: %s", snapshot.session_id.c_str());
    if (snapshot.incomplete) wrapped("This session is incomplete. Inspect the recording error and coverage before comparing results.", red);
}

void gps_settings(Engine& engine, DesktopState& ui, const Snapshot& snapshot) {
    label("FIXED OR MOBILE RECEIVER POSITION");
    if (ui.preferences_active) {
        ImGui::BeginDisabled(snapshot.running);
        if (ImGui::Checkbox("Start GPS automatically", &ui.gps_enabled)) {
            if (!ui.gps_enabled) engine.disconnect_gps();
            else { ui.focus_serial_gps = true; ui.refresh_gps(); if (ui.selected_gps && !ui.passive_smoke) ui.connect_selected_gps(engine); }
            ui.persist_preferences();
        }
        ImGui::EndDisabled();
        wrapped("GPS is optional. Reception continues if it is unavailable; measurements without a valid receiver position cannot be mapped.");
    }
    ImGui::TextColored(secondary, "%s", snapshot.gps_status.c_str());
    wrapped("Positions describe the receiver, not the transmitting device.");
    if (ImGui::BeginTabBar("gpsMode")) {
        if (ImGui::BeginTabItem("Fixed position")) {
            ImGui::SetNextItemWidth(210);
            ImGui::InputDouble("Latitude", &ui.latitude, 0, 0, "%.6f");
            ImGui::SetNextItemWidth(210);
            ImGui::InputDouble("Longitude", &ui.longitude, 0, 0, "%.6f");
            ImGui::Checkbox("Include altitude", &ui.have_altitude);
            if (ui.have_altitude) {
                ImGui::SetNextItemWidth(210);
                ImGui::InputDouble("Altitude / m", &ui.altitude, 0, 0, "%.1f");
            }
            const bool valid = std::isfinite(ui.latitude) && std::isfinite(ui.longitude) && std::abs(ui.latitude) <= 90 && std::abs(ui.longitude) <= 180 && (!ui.have_altitude || std::isfinite(ui.altitude));
            ImGui::BeginDisabled(!valid || snapshot.historical);
            if (ImGui::Button("Use fixed receiver position")) {
                engine.disconnect_gps();
                ui.gps_enabled = false;
                ui.persist_preferences();
                engine.set_fixed_position(ui.latitude, ui.longitude, ui.have_altitude ? std::optional<double>(ui.altitude) : std::nullopt);
                ui.feedback(true, "Fixed receiver position applied. It is labeled as manually configured.");
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Serial GPS", nullptr, ui.focus_serial_gps ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
            ui.focus_serial_gps = false;
            if (ui.preferences_active) {
                wrapped("Recognized USB GPS receivers are found automatically. Other serial devices require an explicit selection. Discovery does not open the ports.");
                ImGui::BeginDisabled(snapshot.running);
                if (ImGui::Button("Refresh GPS devices")) ui.refresh_gps();
                const std::string selected_label = ui.preferences.gps_device_id.empty() ? "Automatic: one recognized GPS" :
                    ui.selected_gps ? ui.gps_devices.devices[*ui.selected_gps].label : "Remembered GPS is unavailable";
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##gpsDevices", selected_label.c_str())) {
                    if (ImGui::Selectable("Automatic: one recognized GPS", ui.preferences.gps_device_id.empty())) {
                        engine.disconnect_gps();
                        ui.preferences.gps_device_id.clear();
                        ui.selected_gps = ui.gps_devices.error.empty() ? select_gps_device(ui.gps_devices.devices, "") : std::nullopt;
                        ui.persist_preferences();
                    }
                    for (size_t i = 0; i < ui.gps_devices.devices.size(); ++i) {
                        const auto& device = ui.gps_devices.devices[i];
                        ImGui::PushID(static_cast<int>(i));
                        const auto pos = ImGui::GetCursorScreenPos();
                        if (ImGui::Selectable("##gpsDevice", ui.preferences.gps_device_id == device.stable_id)) {
                            engine.disconnect_gps();
                            ui.preferences.gps_device_id = device.stable_id;
                            ui.selected_gps = ui.gps_devices.error.empty() ? select_gps_device(ui.gps_devices.devices, device.stable_id) : std::nullopt;
                            ui.persist_preferences();
                        }
                        const std::string row_label = device.label + " (" + device.path + ")";
                        ImGui::GetWindowDrawList()->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text), row_label.c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                if (!ui.gps_devices.error.empty()) wrapped(ui.gps_devices.error.c_str(), red);
                else if (ui.selected_gps) {
                    ImGui::TextWrapped("Selected: %s (%s)", ui.gps_devices.devices[*ui.selected_gps].label.c_str(), ui.gps_devices.devices[*ui.selected_gps].path.c_str());
                    wrapped("Recognized GPS receivers connect on ordinary startup. Connect now to retry.");
                } else wrapped("No unique GPS selected. Connect a GPS, refresh the list, and select it if needed.", amber);
                constexpr unsigned bauds[] = {4800, 9600, 19200, 38400, 57600, 115200};
                int baud_index = 1;
                for (int i = 0; i < 6; ++i) if (static_cast<unsigned>(ui.gps_baud) == bauds[i]) baud_index = i;
                ImGui::SetNextItemWidth(170);
                if (ImGui::Combo("Baud rate", &baud_index, "4800\0 9600\0 19200\0 38400\0 57600\0 115200\0")) {
                    ui.gps_baud = static_cast<int>(bauds[baud_index]);
                    engine.disconnect_gps();
                    ui.persist_preferences();
                }
                ImGui::BeginDisabled(!ui.selected_gps || !ui.gps_devices.error.empty());
                if (ImGui::Button("Connect GPS now")) {
                    ui.gps_enabled = true;
                    ui.persist_preferences();
                    ui.connect_selected_gps(engine);
                }
                ImGui::EndDisabled();
                ImGui::EndDisabled();
            } else {
            wrapped("Explicit test mode: GPS is connected only through the configured port. Ordinary desktop startup provides automatic discovery.");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##gpsPath", "Serial device path", ui.gps_path.data(), ui.gps_path.size());
            ImGui::SetNextItemWidth(170);
            ImGui::InputInt("Baud rate", &ui.gps_baud, 0, 0);
            ImGui::BeginDisabled(ui.gps_path[0] == 0 || ui.gps_baud <= 0 || snapshot.historical);
            if (ImGui::Button("Connect this GPS")) {
                std::string error;
                const bool ok = engine.connect_gps(ui.gps_path.data(), static_cast<unsigned>(ui.gps_baud), error);
                ui.feedback(ok, ok ? "GPS serial source connected. Waiting for a valid fix." : error);
            }
            ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if (ImGui::Button("Disconnect GPS")) engine.disconnect_gps();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::BeginDisabled(snapshot.historical);
    if (ImGui::Button("Clear receiver position")) {
        engine.disconnect_gps();
        engine.clear_position();
        ui.feedback(true, "Receiver position cleared for future measurements.");
    }
    ImGui::EndDisabled();
    position_view_controls(ui);
}

double occupancy_height(double value, OccupancyScale scale) {
    if (!std::isfinite(value)) return 0;
    value = std::clamp(value, 0.0, 1.0);
    // A logarithmic transform offset at zero keeps true zero representable.
    // Percentage ticks and tooltips remain in the original measured units.
    return scale == OccupancyScale::Linear ? value : low_activity_height(value);
}

struct OccupancyColumn {
    size_t first = 0, last = 0, representative = 0;
    bool observed = false, unobserved = false;
    double maximum_ratio = 0;
};

template<class Bin> std::vector<OccupancyColumn> uniform_occupancy_columns(const std::vector<Bin>& bins, size_t pixels) {
    const size_t count = std::min(bins.size(), pixels);
    std::vector<OccupancyColumn> columns;
    columns.reserve(count);
    for (size_t column = 0; column < count; ++column) {
        OccupancyColumn result;
        result.first = column * bins.size() / count;
        result.last = (column + 1) * bins.size() / count;
        result.representative = result.first;
        for (size_t index = result.first; index < result.last; ++index) {
            const auto& bin = bins[index];
            if (bin.observed_seconds <= 0) { result.unobserved = true; continue; }
            const double occupancy = ratio(bin.active_seconds, bin.observed_seconds);
            if (!result.observed || occupancy > result.maximum_ratio) {
                result.maximum_ratio = occupancy;
                result.representative = index;
            }
            result.observed = true;
        }
        columns.push_back(result);
    }
    return columns;
}

template<class Bin> FrequencyRange frequency_extent(const std::vector<Bin>& bins) {
    if (bins.empty()) return {};
    FrequencyRange range{double(bins.front().center_hz) - bins.front().width_hz / 2.0,
                         double(bins.front().center_hz) + bins.front().width_hz / 2.0};
    for (const auto& bin : bins) {
        range.lower_hz = std::min(range.lower_hz, double(bin.center_hz) - bin.width_hz / 2.0);
        range.upper_hz = std::max(range.upper_hz, double(bin.center_hz) + bin.width_hz / 2.0);
    }
    return range;
}

template<class Bin> std::vector<OccupancyColumn> occupancy_columns(const std::vector<Bin>& bins, size_t pixels) {
    if (bins.empty() || !pixels) return {};
    bool uniform = true;
    for (size_t i = 1; i < bins.size(); ++i)
        if (bins[i].width_hz != bins.front().width_hz ||
            std::abs(double(bins[i].center_hz) - double(bins[i-1].center_hz) - bins.front().width_hz) > 1.0)
            uniform = false;
    if (uniform) return uniform_occupancy_columns(bins, pixels);
    // Resumed acquisitions can have gaps and different FFT grids. Draw against
    // real Hz edges; equal spacing by row index would invent covered bandwidth.
    const auto range = frequency_extent(bins);
    const double span = range.upper_hz - range.lower_hz;
    if (!(span > 0)) return {};
    const size_t count = std::min<size_t>(pixels, 8192);
    std::vector<OccupancyColumn> columns(count);
    for (size_t i = 0; i < bins.size(); ++i) {
        const auto& bin = bins[i];
        const double lower = std::clamp((double(bin.center_hz) - bin.width_hz/2.0 - range.lower_hz)/span, 0., 1.);
        const double upper = std::clamp((double(bin.center_hz) + bin.width_hz/2.0 - range.lower_hz)/span, 0., 1.);
        const size_t begin = std::min(count - 1, size_t(std::floor(lower * double(count))));
        const size_t end = std::min(count, size_t(std::ceil(upper * double(count))));
        for (size_t pixel = begin; pixel < end; ++pixel) {
            auto& column = columns[pixel];
            if (column.last == 0) column.first = i;
            column.last = i + 1;
            if (bin.observed_seconds <= 0) { column.unobserved = true; continue; }
            const double busy = ratio(bin.active_seconds, bin.observed_seconds);
            if (!column.observed || busy > column.maximum_ratio) { column.maximum_ratio = busy; column.representative = i; }
            column.observed = true;
        }
    }
    for (auto& column : columns) if (!column.last) column.unobserved = true;
    return columns;
}

template<class Bin> std::optional<FrequencyRange> frequency_gesture_range(const std::vector<Bin>& bins,
        double anchor, double cursor, bool dragging, double click_width_hz) {
    if (bins.empty() || !std::isfinite(anchor) || !std::isfinite(cursor)) return {};
    const auto range = frequency_extent(bins);
    const double lower = range.lower_hz, upper = range.upper_hz;
    if (upper <= lower) return {};
    anchor = std::clamp(anchor, 0.0, 1.0); cursor = std::clamp(cursor, 0.0, 1.0);
    if (dragging) {
        const double left = lower + std::min(anchor, cursor) * (upper - lower);
        const double right = lower + std::max(anchor, cursor) * (upper - lower);
        std::optional<FrequencyRange> selected;
        for (const auto& bin : bins) {
            const double lo = double(bin.center_hz) - bin.width_hz/2.0, hi = double(bin.center_hz) + bin.width_hz/2.0;
            if (hi <= left || lo > right) continue;
            if (!selected) selected = FrequencyRange{lo, hi};
            else { selected->lower_hz = std::min(selected->lower_hz, lo); selected->upper_hz = std::max(selected->upper_hz, hi); }
        }
        return selected ? selected : std::optional<FrequencyRange>{{left, right}};
    }
    if (!std::isfinite(click_width_hz) || click_width_hz <= 0) return {};
    const double width = std::min(click_width_hz, upper - lower);
    const double start = std::clamp(lower + anchor * (upper - lower) - width / 2, lower, upper - width);
    return FrequencyRange{start, start + width};
}

void queue_analysis(DesktopState& ui, const Snapshot& snapshot, bool reveal_time_plot) {
    if (is_concentrator(snapshot.config)) {
        ui.pending_analysis.reset();
        return; // Sampled RSSI uses its own view and reports, never FFT occupancy.
    }
    if (snapshot.config.session_path.empty() || snapshot.session_id.empty()) {
        ui.feedback(false, "A saved survey is required for frequency-linked time and GPS analysis.");
        return;
    }
    auto query = ui.survey_query;
    if (ui.query_lower_mhz == 0 && ui.query_upper_mhz == 0) {
        const auto extent = frequency_extent(snapshot.frequencies);
        ui.query_lower_mhz = extent.lower_hz / 1e6;
        ui.query_upper_mhz = extent.upper_hz / 1e6;
    }
    query.lower_hz = ui.query_lower_mhz * 1e6;
    query.upper_hz = ui.query_upper_mhz * 1e6;
    ui.pending_analysis = PendingAnalysis{query, snapshot.session_id, reveal_time_plot};
    ui.last_analysis_requested_while_running = snapshot.running;
    ui.feedback(true, "Updating frequency, time and GPS analysis from saved measurements...");
}

void select_analysis_frequencies(DesktopState& ui, const Snapshot& snapshot, const FrequencyRange& range) {
    ui.query_lower_mhz = range.lower_hz / 1e6;
    ui.query_upper_mhz = range.upper_hz / 1e6;
    queue_analysis(ui, snapshot, true);
}

void complete_pending_analysis(Engine& engine, DesktopState& ui) {
    if (!ui.pending_analysis) return;
    const auto request = std::move(*ui.pending_analysis);
    ui.pending_analysis.reset();
    // A new session opened or started in the same UI frame must not receive a
    // query captured for the previous file. No receiver configuration is changed.
    if (engine.snapshot().session_id != request.session_id) {
        ui.feedback(false, "The survey changed. Select frequencies again for the current survey.");
        return;
    }
    SurveyAnalysis result;
    std::string error;
    const bool ok = engine.analyze_survey(request.query, result, error);
    ui.feedback(ok, ok ? "Analysis updated. The highlighted frequencies drive the time and GPS plots." : error);
    ui.analysis_loaded = ok;
    ui.selected_observation.reset();
    if (ok) {
        ui.analysis = std::move(result);
        ui.analyzed_query = request.query;
        ui.analysis_session = request.session_id;
        ui.reveal_time_plot = request.reveal_time_plot;
    }
}

// The native event loop never runs database scans on the rendering thread.
// Results are tagged with the requested session and discarded after New/Open.
void progress_analysis(Engine& engine, DesktopState& ui) {
    if (ui.analysis_operation.valid() &&
        ui.analysis_operation.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto result = ui.analysis_operation.get();
        if (engine.snapshot().session_id == result.request.session_id && !ui.pending_analysis) {
            ui.analysis_loaded = result.ok;
            ui.selected_observation.reset();
            if (result.ok) {
                ui.analysis = std::move(result.analysis);
                ui.analyzed_query = result.request.query;
                ui.analysis_session = result.request.session_id;
                ui.reveal_time_plot = result.request.reveal_time_plot;
            } else ui.feedback(false, result.error);
        }
    }
    if (ui.analysis_operation.valid() || !ui.pending_analysis || ui.operation_busy()) return;
    const auto request = std::move(*ui.pending_analysis);
    ui.pending_analysis.reset();
    if (engine.snapshot().session_id != request.session_id) return;
    ui.analysis_operation = std::async(std::launch::async, [&engine, request] {
        CompletedAnalysis result; result.request = request;
        try { result.ok = engine.analyze_survey(request.query, result.analysis, result.error); }
        catch (const std::exception& e) { result.error = e.what(); }
        return result;
    });
}

template<class Bin> void occupancy_chart(const std::vector<Bin>& frequencies, float height,
                                         OccupancyScale scale = OccupancyScale::RevealLowActivity,
                                         double guard_lower_hz = 0, double guard_upper_hz = 0,
                                         FrequencyChartSelection* selection = nullptr) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size{std::max(200.0f, ImGui::GetContentRegionAvail().x), std::max(180.0f, height)};
    ImGui::InvisibleButton("##occupancy", size);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(7, 14, 22, 255), 5);
    const float margin = std::max(54.0f, ImGui::CalcTextSize("0.001%").x + 10);
    const float left = origin.x + margin, right = origin.x + size.x - 12;
    const float top = origin.y + 18, bottom = origin.y + size.y - 30;
    if (selection) selection->requested.reset();
    const std::vector<double> ticks = scale == OccupancyScale::Linear ?
        std::vector<double>{0, .25, .5, .75, 1} : std::vector<double>{0, .00001, .0001, .001, .01, .1, 1};
    for (const auto tick : ticks) {
        const float y = bottom - (bottom - top) * static_cast<float>(occupancy_height(tick, scale));
        draw->AddLine({left, y}, {right, y}, IM_COL32(31, 47, 60, 255));
        std::array<char, 24> text{};
        std::snprintf(text.data(), text.size(), "%g%%", tick * 100);
        draw->AddText({origin.x + 4, y - ImGui::GetFontSize() / 2}, IM_COL32(140, 158, 173, 255), text.data());
    }
    if (frequencies.empty()) {
        if (selection) selection->anchor_fraction.reset();
        draw->AddText({left + 12, top + 28}, IM_COL32(140, 158, 173, 255), "Survey measurements appear after reception starts.");
        return;
    }
    const auto extent_hz = frequency_extent(frequencies);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const auto& io = ImGui::GetIO();
    const auto fraction_at = [&](float x) { return std::clamp(double((x - left) / (right - left)), 0.0, 1.0); };
    std::optional<FrequencyRange> preview;
    if (selection) {
        const bool inside = io.MousePos.x >= left && io.MousePos.x <= right && io.MousePos.y >= top && io.MousePos.y <= bottom;
        if (hovered && inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            selection->anchor_fraction = fraction_at(io.MousePos.x);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) selection->anchor_fraction.reset();
        if (selection->anchor_fraction) {
            const bool dragged = std::abs(io.MousePos.x - io.MouseClickedPos[0].x) >= io.MouseDragThreshold;
            preview = frequency_gesture_range(frequencies, *selection->anchor_fraction, fraction_at(io.MousePos.x),
                                              dragged, selection->click_width_hz);
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                selection->requested = preview;
                selection->anchor_fraction.reset();
            } else if (!active || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) selection->anchor_fraction.reset();
        }
    }
    const auto columns = occupancy_columns(frequencies, static_cast<size_t>(std::max(1.0f, std::floor(right - left))));
    const float column_width = (right - left) / static_cast<float>(columns.size());
    for (size_t i = 0; i < columns.size(); ++i) {
        const auto& column = columns[i];
        const float x = left + column_width * static_cast<float>(i);
        if (column.observed && column.maximum_ratio > 0) {
            const float h = static_cast<float>(occupancy_height(column.maximum_ratio, scale)) * (bottom - top);
            draw->AddRectFilled({x, bottom - std::max(2.0f, h)}, {x + column_width, bottom}, IM_COL32(62, 195, 176, 255));
        }
        // Unknown observation is not a measured zero; retain it even when
        // an adjacent observed bin shares the same horizontal display column.
        if (column.unobserved)
            draw->AddRectFilled({x, bottom + 2}, {x + column_width, bottom + 4}, IM_COL32(129, 92, 63, 255));
    }
    if (guard_upper_hz > guard_lower_hz) {
        const double first_edge = extent_hz.lower_hz;
        const double last_edge = extent_hz.upper_hz;
        if (last_edge > first_edge && guard_upper_hz > first_edge && guard_lower_hz < last_edge) {
            const auto x_at = [&](double hz) { return left + float(std::clamp((hz - first_edge) / (last_edge - first_edge), 0.0, 1.0)) * (right - left); };
            const float x0 = x_at(guard_lower_hz), x1 = std::max(x0 + 2, x_at(guard_upper_hz));
            draw->AddRectFilled({x0, top}, {std::min(right, x1), bottom}, IM_COL32(255, 170, 76, 100));
        }
    }
    const auto outline_range = [&](const FrequencyRange& range, ImU32 color, float thickness) {
        const double first_edge = extent_hz.lower_hz;
        const double last_edge = extent_hz.upper_hz;
        if (last_edge <= first_edge || range.upper_hz <= first_edge || range.lower_hz >= last_edge) return;
        const auto x_at = [&](double hz) { return left + float(std::clamp((hz - first_edge) / (last_edge - first_edge), 0.0, 1.0)) * (right - left); };
        draw->AddRect({x_at(range.lower_hz), top}, {x_at(range.upper_hz), bottom}, color, 0, 0, thickness);
    };
    if (selection && selection->highlighted) outline_range(*selection->highlighted, IM_COL32(114, 173, 240, 255), 2);
    if (preview) outline_range(*preview, IM_COL32(245, 245, 245, 255), 2);
    for (int tick = 0; tick < 5; ++tick) {
        std::array<char, 32> text{};
        std::snprintf(text.data(), text.size(), "%.3f MHz", (extent_hz.lower_hz + (extent_hz.upper_hz - extent_hz.lower_hz) * tick / 4) / 1e6);
        const ImVec2 extent = ImGui::CalcTextSize(text.data());
        const float x = left + (right - left) * static_cast<float>(tick) / 4;
        draw->AddText({std::clamp(x - extent.x / 2, left, std::max(left, right - extent.x)), bottom + 9}, IM_COL32(150, 173, 190, 255), text.data());
    }
    if (ImGui::IsItemHovered()) {
        const float fraction = std::clamp((ImGui::GetIO().MousePos.x - left) / (right - left), 0.0f, 0.999999f);
        const auto& column = columns[static_cast<size_t>(fraction * static_cast<float>(columns.size()))];
        const auto& bin = frequencies[column.representative];
        const auto& first = frequencies[column.first];
        const auto& last = frequencies[column.last ? column.last - 1 : 0];
        ImGui::BeginTooltip();
        if (preview) ImGui::Text("Release to analyze %.6f - %.6f MHz / %.3f kHz (Esc cancels)",
            preview->lower_hz / 1e6, preview->upper_hz / 1e6, (preview->upper_hz - preview->lower_hz) / 1e3);
        else if (selection) ImGui::Text("Click: %.3f kHz view. Drag: select frequency edges for time/GPS analysis.", selection->click_width_hz / 1e3);
        if (!column.last) {
            ImGui::TextUnformatted("No recorded frequency coverage at this point.");
            ImGui::TextUnformatted("This gap is unobserved, not a quiet channel.");
            ImGui::EndTooltip(); return;
        }
        ImGui::Text("Displayed interval %.6f - %.6f MHz (%zu bins)",
            (static_cast<double>(first.center_hz) - first.width_hz / 2.0) / 1e6,
            (static_cast<double>(last.center_hz) + last.width_hz / 2.0) / 1e6, column.last - column.first);
        if (column.observed) {
            ImGui::Text("Strongest bin %.6f MHz / %.3f kHz", static_cast<double>(bin.center_hz) / 1e6, bin.width_hz / 1e3);
            ImGui::Text("Measured occupancy %#.6g%%", column.maximum_ratio * 100);
            ImGui::Text("Observed %.3f s | busy %.6f s", bin.observed_seconds, bin.active_seconds);
            ImGui::Text("Mean %.1f dBFS | peak %.1f dBFS", bin.mean_dbfs, bin.peak_dbfs);
            ImGui::TextUnformatted("Maximum single-bin occupancy, not combined interval busy time.");
        }
        if (column.unobserved) ImGui::TextUnformatted("Contains unobserved bins; missing observation is not zero occupancy.");
        if (guard_upper_hz > guard_lower_hz &&
            double(last.center_hz) + last.width_hz / 2.0 > guard_lower_hz &&
            double(first.center_hz) - first.width_hz / 2.0 < guard_upper_hz)
            ImGui::TextUnformatted("Receiver-center guard overlaps this column. Original measurements remain shown; a receiver artifact is possible, not confirmed.");
        ImGui::EndTooltip();
    }
}

std::string quality_text(uint32_t flags) {
    std::string result;
    const auto add = [&](uint32_t flag, const char* name) {
        if (flags & flag) { if (!result.empty()) result += ", "; result += name; }
    };
    add(SurveyUncalibrated, "uncalibrated");
    add(SurveyUpstreamLossUnknown, "upstream loss unknown");
    add(SurveyClipped, "clipped");
    add(SurveyPositionMissing, "position missing");
    add(SurveyBoundary, "measurement boundary");
    add(SurveyTruncated, "truncated");
    add(SurveyMerged, "merged");
    add(SurveyBackgroundUncertain, "background uncertain");
    add(SurveyDcSuspect, "possible DC artifact");
    add(SurveyPowerAggregated, "power aggregated");
    return result.empty() ? "none recorded" : result;
}

std::string quality_explanation(uint32_t flags) {
    std::string text;
    const auto add = [&](uint32_t bit, const char* meaning) {
        if (flags & bit) { if (!text.empty()) text += "\n\n"; text += meaning; }
    };
    add(SurveyUncalibrated, "Uncalibrated: power is relative dBFS. It is not calibrated dBm or a measured field strength.");
    add(SurveyUpstreamLossUnknown, "Upstream loss unknown: the receiver cannot prove whether samples were lost before reaching the application. This does not report a detected loss.");
    add(SurveyClipped, "Clipped: samples reached the ADC-near-limit criterion. Power and activity may be distorted.");
    add(SurveyPositionMissing, "Position missing: at least one required receiver-position association is absent. An event can lack a start or end fix even while the GPS now reports a fix. RF observed time remains recorded.");
    add(SurveyBoundary, "Measurement boundary: an event touches the measured frequency edge, or an analysis interval cuts a stored tile. Activity beyond recorded edges is unknown. Partial-tile power uses the tile mean; activity timing retains the recorded FFT resolution.");
    add(SurveyTruncated, "Truncated / segmented: an energy event was split at the approximately two-second limit, a stop/gap or a processing bound. This flag alone does not distinguish the cause or prove lost RF data. Check coverage gaps separately.");
    add(SurveyMerged, "Merged: multiple energy components or measurement buckets were combined. This is not a count of transmitters or packets.");
    add(SurveyBackgroundUncertain, "Background uncertain: the lower spectral-percentile estimate is not a verified noise-only reference. It does not adjust the fixed activity threshold.");
    add(SurveyDcSuspect, "Possible DC artifact: the event overlaps the receiver-center region. Internal receiver energy is possible, but real RF may also occupy this region.");
    add(SurveyPowerAggregated, "Power aggregated: saved power summaries combine roughly one second of measurements. A shorter selection reuses that summary's power; exact 20 ms power detail cannot be reconstructed. Recorded activity timing and original GPS fixes retain their precision.");
    return text.empty() ? "No quality flags recorded. This is not a calibration or completeness guarantee." : text;
}

void quality_help(uint32_t flags) {
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
        ImGui::TextUnformatted(quality_explanation(flags).c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void waveform_panel(const Snapshot& snapshot, float height, bool configured_for_next = false,
                    const SurveyAnalysis* selected = nullptr) {
    label("LORA WAVEFORM OBSERVATIONS / INFERRED MODEM SETTINGS");
    const auto& status = snapshot.discovery;
    const auto& observations = selected ? selected->waveforms : snapshot.waveforms;
    if (!status.enabled && !status.failed && snapshot.waveforms.empty() && observations.empty()) {
        wrapped(configured_for_next && !snapshot.running && !snapshot.historical
            ? "Waveform discovery is configured for the next survey. Start reception to search the supplied frequency range."
            : "No waveform discovery results for this session. Enable Discover LoRa waveforms before starting a new survey.");
        wrapped("Energy-group widths below cannot identify LoRa bandwidth or spreading factor.", amber);
        return;
    }
    wrapped("Experimental (live RF not yet validated). Mesh protocol: unknown | Payload: not decoded", amber);
    const char* state = snapshot.historical ? "saved session" : status.failed ? "discovery failed" :
        status.finished ? "discovery finished" : "search active";
    if (selected) {
        ImGui::TextWrapped("%zu shown | %llu waveform observations match the latest completed analysis query | %s",
            observations.size(), static_cast<unsigned long long>(selected->waveform_count), state);
        if (selected->waveforms_truncated) wrapped("The selected result list is limited; additional matching observations are not displayed.", amber);
    } else ImGui::TextWrapped("%zu recent shown | %llu observations reported | full session, unfiltered | %s",
        observations.size(), static_cast<unsigned long long>(status.observations), state);
    if (status.failed) {
        wrapped("WAVEFORM DISCOVERY FAILED. Results are incomplete.", red);
        if (!status.fault.empty()) ImGui::TextWrapped("Discovery failure: %s", status.fault.c_str());
    }
    const bool lost = status.rejected_input_samples || status.abandoned_input_samples || status.source_queue_drops ||
        status.result_overflows || status.gap_overflows;
    if (lost) {
        wrapped("DISCOVERY COVERAGE LOSS: signals may be missing. See discovery counters below; spectrum occupancy has separate coverage.", amber);
    } else if (status.stream_resets) {
        ImGui::TextColored(amber, "Discovery stream resets: %llu; preamble history was cleared",
            static_cast<unsigned long long>(status.stream_resets));
    }
    uint64_t candidate_limits = 0, track_limits = 0, abandoned_band_samples = 0;
    bool subband_gaps = false;
    for (const auto& band : status.bands) {
        candidate_limits += band.candidate_limit_hits;
        track_limits += band.track_limit_hits;
        abandoned_band_samples += band.abandoned_samples;
        subband_gaps = subband_gaps || band.source_gap_input_samples != 0;
    }
    if (candidate_limits || track_limits || abandoned_band_samples)
        ImGui::TextColored(amber, "Bounded search pressure: %llu candidate limits | %llu track limits | %llu abandoned subband samples",
            static_cast<unsigned long long>(candidate_limits), static_cast<unsigned long long>(track_limits),
            static_cast<unsigned long long>(abandoned_band_samples));
    if (subband_gaps) wrapped("Discovery subband gaps are recorded; see processing coverage. Preamble history is interrupted by gaps.", amber);
    if (observations.empty()) {
        wrapped(selected ? "No waveform observations match this query. This does not establish that the selected range was quiet."
            : "No matching waveform observations yet. This does not establish that the range is quiet.");
    }
    if (!observations.empty() && ImGui::BeginTable("waveformObservations", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, {0, std::max(80.0f, height)})) {
        ImGui::TableSetupColumn("Center MHz");
        ImGui::TableSetupColumn("Inferred BW kHz");
        ImGui::TableSetupColumn("SF");
        ImGui::TableSetupColumn("Observed at (UTC)", ImGuiTableColumnFlags_WidthFixed,
            ImGui::CalcTextSize("0000-00-00 00:00:00.000").x + 12);
        ImGui::TableSetupColumn("Delimiter / s");
        ImGui::TableSetupColumn("Observation ID");
        ImGui::TableSetupColumn("Evidence scope");
        ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
        for (const auto& observation : observations) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.6f", observation.center_hz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%u", observation.bandwidth_hz / 1000);
            ImGui::TableNextColumn(); ImGui::Text("SF%u", observation.spreading_factor);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(timestamp_text(observation.delimiter_utc).c_str());
            reception_time_tooltip(observation.delimiter_utc, observation.delimiter_elapsed, true);
            ImGui::TableNextColumn(); ImGui::Text("%.6f", observation.delimiter_elapsed);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("First observed preamble evidence: %.6f s\nDelimiter evidence: %.6f s\nThese are observation times, not packet start/end or total airtime.",
                observation.first_observed_elapsed, observation.delimiter_elapsed);
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(observation.id));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Consolidated waveform observation, not a unique packet or transmitter.\nContributing processing subbands: %u\nUp/down matched fractions of subband power: %.4g / %.4g\nThese are not confidence probabilities; other signals can lower the fractions.",
                observation.contributing_subbands, observation.up_match, observation.down_match);
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s%s",
                observation.complete_in_requested_range ? "Within survey range" : "Range edge / incomplete",
                observation.association_ambiguous ? "; association ambiguous" : "");
        }
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Waveform interpretation and discovery counters")) {
        wrapped("Inferred BW is matched LoRa modem bandwidth, not an energy-group width. Delimiter time is elapsed since survey start; observed preamble timing is not packet airtime. Waveform matching does not identify Meshtastic, MeshCore or LoRaWAN, and is not an exhaustive signal inventory.");
        wrapped("A waveform can also appear in receiver or transmitter images. Coincident observations at different frequencies are not proof of separate emitters. Compare receiver tuning or independent equipment before attributing a weak companion to local spectrum use.");
        wrapped(selected ? "Rows use the latest completed query's inferred frequency-band overlap, delimiter time and receiver-position filters."
            : "Rows are recent session observations across the supplied frequency range, not filtered by an analysis selection.");
        wrapped("Discovery counters describe the whole session and are separate from spectrum occupancy coverage. Queue counts are processing events, not packet counts.");
        wrapped("Candidate limits include competing preamble-reset fits. Limits can occur even when a waveform is recovered; they do not count known lost packets. Abandoned samples and rejected input are separate processing losses.");
        wrapped("RF acquisition gaps also interrupt discovery, including leading/trailing gaps that may not appear in worker subband counters. Reduce the survey range if discovery falls behind.");
        ImGui::TextWrapped("Rejected input: %llu samples | abandoned input: %llu samples | queue drops: %llu",
            static_cast<unsigned long long>(status.rejected_input_samples),
            static_cast<unsigned long long>(status.abandoned_input_samples),
            static_cast<unsigned long long>(status.source_queue_drops));
        ImGui::TextWrapped("Result overflows: %llu | gap-report overflows: %llu | stream resets: %llu",
            static_cast<unsigned long long>(status.result_overflows),
            static_cast<unsigned long long>(status.gap_overflows), static_cast<unsigned long long>(status.stream_resets));
        ImGui::TextWrapped("Accepted input: %llu samples | channelized input: %llu samples | %zu processing subbands",
            static_cast<unsigned long long>(status.accepted_input_samples),
            static_cast<unsigned long long>(status.channelized_input_samples), status.bands.size());
        wrapped("Accepted input is not necessarily processed. Subband samples are at 2 MS/s; source-gap samples use the survey input rate. Overlapping subbands must not be added as independent RF coverage.");
        for (const auto& band : status.bands) ImGui::TextWrapped(
            "Subband %u at %.6f MHz: processed %llu | abandoned %llu | source gap %llu input samples",
            band.subband_index, band.center_hz / 1e6,
            static_cast<unsigned long long>(band.processed_samples), static_cast<unsigned long long>(band.abandoned_samples),
            static_cast<unsigned long long>(band.source_gap_input_samples));
    }
}

void energy_event_table(const std::vector<SpectrumEvent>& events, float height) {
    ImGui::TextUnformatted("RAW ENERGY FRAGMENTS / DETAIL");
    wrapped("Connected above-threshold energy, not packets. Measured width is an energy envelope, not a modem bandwidth; overlapping signals may merge and chirps may fragment.");
    wrapped("Continuous activity is split into approximately two-second events. Hover a Quality cell for explanations; event durations must not be added to calculate channel occupancy.");
    if (ImGui::BeginTable("energyEvents", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, {0, std::max(90.0f, height)})) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* column : {"Start / s", "End / s", "Lower MHz", "Upper MHz", "Width kHz", "Duration ms", "Peak dBFS", "Quality"})
            ImGui::TableSetupColumn(column);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(events.size()));
        while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto& event = events[static_cast<size_t>(i)];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.3f", event.elapsed_start_seconds);
            ImGui::TableNextColumn(); ImGui::Text("%.3f", event.elapsed_end_seconds);
            ImGui::TableNextColumn(); ImGui::Text("%.6f", event.lower_hz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.6f", event.upper_hz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", (event.upper_hz - event.lower_hz) / 1000);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", (event.elapsed_end_seconds - event.elapsed_start_seconds) * 1000);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Active %.3f ms | mean %.1f dBFS", event.active_seconds * 1000, event.mean_dbfs);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", event.peak_dbfs);
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", quality_text(event.quality).c_str());
            quality_help(event.quality);
        }
        ImGui::EndTable();
    }
    if (events.empty()) ImGui::TextDisabled("No completed energy events in this view.");
}

void burst_table(const std::vector<SpectrumBurst>& bursts, uint64_t total, float height) {
    const float heading_y = ImGui::GetCursorPosY();
    label("ENERGY GROUPS / SUPPORTING DETAIL");
    wrapped(desktop_lora_enabled ? "Energy width describes RF activity. For estimated LoRa bandwidth and spreading factor (SF), use Detected signals."
        : "Energy width is the measured frequency extent of RF activity. Events are not packets or devices; the width is not a decoded modem setting.");
    help("Energy groups are provisional, not packets or modem settings. They can split a transmission or combine overlapping activity.\nAt least 2 active FFTs per seed bin in ~20 ms; up to 40 ms quiet gaps and 2-bin frequency gaps.\nCenter artifacts remain separate. Show raw fragments reveals the underlying energy events.");
    ImGui::Text("%llu completed groups | %zu shown | center-region activity kept separate",
        static_cast<unsigned long long>(total), bursts.size());
    if (bursts.empty()) wrapped("Groups appear after a short quiet interval, a 10-second segment limit, or Stop reception.");
    if (ImGui::BeginTable("candidateBursts", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, {0,std::max(70.0f,height - (ImGui::GetCursorPosY() - heading_y))})) {
        ImGui::TableSetupScrollFreeze(0,1);
        for (const char* name : {"Start / s", "Lower MHz", "Upper MHz", "Width kHz", "Duration ms", "Active ms", "Tile peak upper bound dBFS", "Grouping"})
            ImGui::TableSetupColumn(name);
        ImGui::TableHeadersRow();
        for (const auto& b : bursts) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%.3f",b.elapsed_start_seconds);
            ImGui::TableNextColumn(); ImGui::Text("%.6f",b.lower_hz/1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.6f",b.upper_hz/1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.2f",(b.upper_hz-b.lower_hz)/1e3);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Measured frequency width of this candidate group.\nThis is not a decoded modem setting or a 99%% occupied-power bandwidth.\nThreshold, FFT resolution, signal strength and overlapping signals affect the edges.");
            ImGui::TableNextColumn(); ImGui::Text("%.2f",(b.elapsed_end_seconds-b.elapsed_start_seconds)*1000);
            ImGui::TableNextColumn(); ImGui::Text("%.2f",b.active_seconds*1000);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Union of active FFT intervals assigned to this group; excludes quiet gaps.\nGroups may overlap: use the frequency occupancy chart for channel busy time.");
            ImGui::TableNextColumn(); ImGui::Text("%.1f",b.peak_dbfs);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Maximum recorded bin peak from the contributing measurement tiles.\nThe peak may occur outside this group's active times or a selected partial tile;\nit is an upper bound, not a time-resolved measurement of this transmission's peak.");
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s%s%s",b.center_region?"Center region; ":"Candidate; ",
                b.ambiguous?"ambiguous; ":"",b.limited?"boundary / segmented":"heuristic");
            quality_help(b.quality);
        }
        ImGui::EndTable();
    }
}

void live_burst_table(DesktopState& ui, const Snapshot& snapshot, float height) {
    auto& list = ui.burst_list;
    if (snapshot.historical) {
        ImGui::Checkbox("Show raw fragments", &list.show_fragments);
        if (list.show_fragments) energy_event_table(snapshot.recent_spectrum_events,height);
        else if (ui.analysis_loaded && ui.analysis_session==snapshot.session_id) {
            wrapped("Saved candidate groups for the latest analysis selection. Change the frequency/time selection in Analyze & export.");
            burst_table(ui.analysis.bursts,ui.analysis.burst_count,height);
        } else wrapped("Run saved analysis in Analyze & export to reconstruct candidate groups from the recorded measurements.");
        return;
    }
    ImGui::Checkbox("Pause result list", &list.paused);
    help("Pauses this table only. Reception and recording continue; the table normally refreshes once per second.");
    ImGui::SameLine(); ImGui::Checkbox("Show raw fragments", &list.show_fragments);
    list.update(snapshot, ImGui::GetTime());
    if (list.show_fragments) energy_event_table(list.fragments,height);
    else burst_table(list.bursts,list.count,height);
}

void select_observation(DesktopState& ui, size_t index) {
    ui.selected_observation = index;
    const auto& observation = ui.analysis.observations[index];
    ui.survey_query.elapsed_start = observation.elapsed_start;
    ui.survey_query.elapsed_end = observation.elapsed_end;
    ui.feedback(true, "Selected observation copied to the elapsed interval. Run analysis to apply the selection.");
}

bool guarded_view(const DesktopState& ui) {
    return ui.use_center_guard && ui.analysis.center_guard_bin_count > 0;
}

std::optional<double> displayed_busy_ratio(const DesktopState& ui, const SurveyObservation& observation) {
    const bool guarded = guarded_view(ui);
    const double observed = guarded ? observation.outside_center_observed_seconds : observation.observed_seconds;
    if (observation.observed_seconds <= 0 || observed <= 0 || (guarded && ui.analysis.outside_center_bin_count == 0)) return {};
    return ratio(guarded ? observation.outside_center_busy_seconds : observation.busy_seconds, observed);
}

std::string analysis_frequency_text(const SurveyAnalysis& result) {
    if (result.covered_upper_hz <= result.covered_lower_hz) return "Frequency range: no recorded bins in this selection";
    std::array<char, 180> text{};
    std::snprintf(text.data(), text.size(), "Frequency range: %.6f - %.6f MHz | Width: %.3f kHz",
        result.covered_lower_hz / 1e6, result.covered_upper_hz / 1e6,
        (result.covered_upper_hz - result.covered_lower_hz) / 1e3);
    return text.data();
}

void analysis_frequency_caption(const DesktopState& ui) {
    wrapped(analysis_frequency_text(ui.analysis).c_str(), secondary);
    if (guarded_view(ui) && ui.analysis.mixed_acquisitions) wrapped("The excluded center region follows each acquisition; guarded exposure remains unassessed.");
    else if (guarded_view(ui)) ImGui::TextWrapped("Excluded center region: %.6f - %.6f MHz (%.3f kHz; unassessed)",
        ui.analysis.center_guard_lower_hz / 1e6, ui.analysis.center_guard_upper_hz / 1e6,
        (ui.analysis.center_guard_upper_hz - ui.analysis.center_guard_lower_hz) / 1e3);
    else ImGui::TextWrapped("All measured frequencies inside this interval contribute.");
    if (ui.analysis_busy()) wrapped("Updating selection... these plots still show the frequency range labeled above.", amber);
}

void busy_time_chart(DesktopState& ui, float height) {
    const auto& result = ui.analysis;
    const auto& observations = result.observations;
    label("CHANNEL OCCUPANCY OVER TIME");
    analysis_frequency_caption(ui);
    ImGui::TextWrapped("Each bar: occupied time / observed time in a %.6g-second bucket. Vertical axis: occupancy (%%). Horizontal axis: elapsed seconds.",
        result.effective_time_bucket_seconds);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size{std::max(200.0f, ImGui::GetContentRegionAvail().x), height};
    ImGui::InvisibleButton("##busyTime", size);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(7, 14, 22, 255), 5);
    const float left = origin.x + 44, right = origin.x + size.x - 14;
    const float top = origin.y + 18, bottom = origin.y + size.y - 30;
    for (int n = 0; n <= 4; ++n) {
        const float y = bottom - (bottom - top) * n / 4;
        draw->AddLine({left, y}, {right, y}, IM_COL32(31, 47, 60, 255));
        const std::string value = std::to_string(n * 25) + "%";
        draw->AddText({origin.x + 4, y - 6}, IM_COL32(140, 158, 173, 255), value.c_str());
    }
    const double first = result.resolved_elapsed_start, last = result.resolved_elapsed_end;
    if (last <= first) {
        draw->AddText({left + 8, top + 16}, IM_COL32(140, 158, 173, 255), "No recorded time in this selection.");
        return;
    }
    const double span = last - first;
    const auto x_at = [&](double time) { return left + static_cast<float>(std::clamp((time - first) / span, 0.0, 1.0)) * (right - left); };
    std::optional<size_t> hovered;
    const auto mouse = ImGui::GetIO().MousePos;
    for (size_t i = 0; i < observations.size(); ++i) {
        const auto& observation = observations[i];
        const float x0 = x_at(observation.elapsed_start), x1 = x_at(observation.elapsed_end);
        const auto busy = displayed_busy_ratio(ui, observation);
        const ImU32 color = (observation.quality & SurveyClipped) ? IM_COL32(255, 170, 76, 255) : IM_COL32(62, 195, 176, 255);
        if (busy) {
            // Observed-zero gets an exposure marker below the axis, not a
            // nonzero activity bar. Unavailable guarded selections stay blank.
            draw->AddRectFilled({x0, bottom + 2}, {std::min(right, std::max(x0 + 1, x1)), bottom + 4}, IM_COL32(90, 115, 133, 255));
            if (*busy > 0) {
                const float h = static_cast<float>(*busy) * (bottom - top);
                draw->AddRectFilled({x0, bottom - std::max(1.0f, h)}, {std::min(right, std::max(x0 + 1, x1)), bottom}, color);
            }
        }
        if (ImGui::IsItemHovered() && mouse.x >= x0 && mouse.x <= x1) hovered = i;
    }
    const CoverageGap* hovered_gap = nullptr;
    // Rasterize the union at display resolution. Overlapping saved gaps must
    // not multiply hatch primitives or drawing-buffer allocation each frame.
    // The source records and their exact times remain available for details.
    const size_t gap_columns = static_cast<size_t>(std::clamp(std::ceil(right - left), 1.0f, 4096.0f));
    std::array<int, 4097> gap_edges{};
    for (const auto& gap : result.gaps) {
        if (gap.elapsed_end_seconds <= first || gap.elapsed_start_seconds >= last) continue;
        const float x0 = x_at(gap.elapsed_start_seconds), x1 = x_at(gap.elapsed_end_seconds);
        const auto a = static_cast<size_t>(std::clamp(std::floor((x0 - left) / (right - left) * gap_columns), 0.0f, static_cast<float>(gap_columns)));
        const auto b = static_cast<size_t>(std::clamp(std::ceil((x1 - left) / (right - left) * gap_columns), 0.0f, static_cast<float>(gap_columns)));
        if (a < b) { ++gap_edges[a]; --gap_edges[b]; }
        if (ImGui::IsItemHovered() && mouse.x >= x0 && mouse.x <= x1) hovered_gap = &gap;
    }
    int overlap = 0;
    size_t gap_start = 0;
    for (size_t column = 0; column <= gap_columns; ++column) {
        const int next = overlap + gap_edges[column];
        if (!overlap && next) gap_start = column;
        if (overlap && !next) {
            const float x0 = left + (right - left) * static_cast<float>(gap_start) / gap_columns;
            const float x1 = left + (right - left) * static_cast<float>(column) / gap_columns;
            draw->AddRectFilled({x0, top}, {x1, bottom}, IM_COL32(40, 46, 58, 230));
            // At most one hatch per six columns, even on oversized displays.
            const float step = std::max(6.0f, (right - left) / gap_columns * 6);
            for (float x = x0; x < x1; x += step) draw->AddLine({x, top}, {x, bottom}, IM_COL32(100, 105, 115, 130));
        }
        overlap = next;
    }
    for (int n = 0; n <= 4; ++n) {
        char text[40];
        std::snprintf(text, sizeof(text), "%.2f s", first + span * n / 4);
        draw->AddText({std::clamp(x_at(first + span * n / 4) - 20, left, std::max(left, right - 55)), bottom + 9}, IM_COL32(140, 158, 173, 255), text);
    }
    if (guarded_view(ui) && !result.outside_center_bin_count)
        draw->AddText({left + 8, top + 16}, IM_COL32(225, 180, 115, 255), "Unavailable: the entire selection is inside the guard.");
    else if (observations.empty())
        draw->AddText({left + 8, top + 16}, IM_COL32(140, 158, 173, 255), "No plotted observations in this selection.");
    if (hovered_gap) {
        ImGui::SetTooltip("Recorded coverage gap %.3f - %.3f s\n%s\nUnobserved, not quiet.",
            hovered_gap->elapsed_start_seconds, hovered_gap->elapsed_end_seconds, hovered_gap->reason.c_str());
    } else if (hovered) {
        const auto& observation = observations[*hovered];
        const auto busy = displayed_busy_ratio(ui, observation);
        ImGui::BeginTooltip();
        ImGui::Text("Elapsed %.3f - %.3f s | observed %.6f s", observation.elapsed_start, observation.elapsed_end,
            guarded_view(ui) ? observation.outside_center_observed_seconds : observation.observed_seconds);
        if (busy) ImGui::Text("%s busy %.6f s (%#.6g%%)", guarded_view(ui) ? "Outside guard" : "All selected bins",
            guarded_view(ui) ? observation.outside_center_busy_seconds : observation.busy_seconds, *busy * 100);
        else ImGui::TextUnformatted("Displayed busy time is unavailable.");
        ImGui::Text("Recorded all-bin busy %.6f s", observation.busy_seconds);
        ImGui::Text("Whole-selection power: background %.1f, mean %.1f, peak envelope %.1f dBFS",
            observation.background_dbfs, observation.mean_dbfs, observation.peak_dbfs);
        ImGui::TextWrapped("%s", quality_text(observation.quality).c_str());
        ImGui::TextUnformatted("Click to select this time interval");
        ImGui::EndTooltip();
        if (ImGui::IsItemClicked()) select_observation(ui, *hovered);
    }
    wrapped("Busy means at least one included frequency bin was active, not that all frequencies were occupied. Gray marks below the axis show observed time; striped intervals are recorded gaps. Buckets may combine partial coverage; hover for exposure. Click a bucket, then Run analysis to inspect that interval.");
}

void geographic_rf_view(DesktopState& ui, float height) {
    label("GEOGRAPHIC RF VIEW / OFFLINE RECEIVER POSITIONS");
    analysis_frequency_caption(ui);
    ImGui::PushID("geographicRF");
    position_view_controls(ui);
    wrapped(guarded_view(ui) ?
        "Colors use busy time OUTSIDE the receiver-center guard: cyan 0% / orange 100%; gray is unavailable. The excluded frequencies remain unassessed." :
        "Colors use busy time across ALL selected bins: cyan 0% / orange 100%; gray is unavailable. Color does not describe GPS accuracy or movement.");
    const auto& observations = ui.analysis.observations;
    std::vector<const PositionFix*> fixes;
    for (const auto& observation : observations)
        if (observation.receiver_position) fixes.push_back(&*observation.receiver_position);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size{std::max(200.0f, ImGui::GetContentRegionAvail().x), std::max(160.0f, height)};
    ImGui::InvisibleButton("##geoRF", size);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(7, 14, 22, 255), 5);
    const ImVec2 plot_origin{origin.x + 20, origin.y + 12};
    const ImVec2 plot_size{size.x - 40, size.y - 58};
    const GeographicPlot plot(fixes, plot_size.x, plot_size.y);
    if (!plot.drawable) {
        draw->AddText({origin.x + 18, origin.y + 26}, IM_COL32(140, 158, 173, 255), "No valid receiver positions in the selected observations.");
        ImGui::PopID();
        return;
    }
    position_plot_guides(plot, plot_origin, plot_size);
    std::optional<size_t> hovered;
    std::optional<ImVec2> previous;
    double previous_end = 0;
    bool previous_manual = false;
    float closest = 100;
    const auto mouse = ImGui::GetIO().MousePos;
    for (size_t i = 0; i < observations.size(); ++i) {
        const auto& observation = observations[i];
        const auto projected = observation.receiver_position ? plot.point(*observation.receiver_position) : std::nullopt;
        if (!projected) { previous.reset(); continue; }
        const auto p = position_plot_point(*projected, plot_origin);
        const auto& fix = *observation.receiver_position;
        const double gap = observation.elapsed_start - previous_end;
        if (ui.position_view_mode == PositionViewMode::Mobile && previous &&
            std::isfinite(gap) && gap >= 0 && gap < ui.analysis.effective_time_bucket_seconds * 2 && fix.manual == previous_manual)
            draw->AddLine(*previous, p, IM_COL32(84, 110, 134, 140), 1.5f);
        const auto displayed = displayed_busy_ratio(ui, observation);
        const float busy = static_cast<float>(displayed.value_or(0));
        const ImU32 color = displayed ?
            ImGui::ColorConvertFloat4ToU32({.22f + .76f * busy, .72f - .35f * busy, .82f - .62f * busy, 1}) : IM_COL32(140, 158, 173, 255);
        draw->AddCircleFilled(p, 3, color);
        if (ui.selected_observation && *ui.selected_observation == i) draw->AddCircle(p, 7, IM_COL32(255, 255, 255, 255), 0, 2);
        const float distance = (p.x - mouse.x) * (p.x - mouse.x) + (p.y - mouse.y) * (p.y - mouse.y);
        if (distance < closest) { closest = distance; hovered = i; }
        previous = p; previous_end = observation.elapsed_end; previous_manual = fix.manual;
    }
    if (ui.position_view_mode == PositionViewMode::Stationary) position_mean_marker(plot, plot_origin);
    if (ImGui::IsItemHovered() && hovered) {
        const auto& observation = observations[*hovered];
        ImGui::BeginTooltip();
        position_quality_tooltip(*observation.receiver_position);
        ImGui::Text("Elapsed %.3f - %.3f s", observation.elapsed_start, observation.elapsed_end);
        const auto displayed = displayed_busy_ratio(ui, observation);
        if (displayed) ImGui::Text("%s busy %#.6g%% | observed %.3f s", guarded_view(ui) ? "Outside guard" : "All selected bins", *displayed * 100,
            guarded_view(ui) ? observation.outside_center_observed_seconds : observation.observed_seconds);
        else ImGui::TextUnformatted("Busy: unavailable (unobserved or all frequencies guarded)");
        ImGui::Text("Recorded all-bin busy %.6f s", observation.busy_seconds);
        ImGui::TextUnformatted("Click to select time interval");
        ImGui::EndTooltip();
        if (ImGui::IsItemClicked()) select_observation(ui, *hovered);
    }
    position_spread_text(plot);
    if (ui.selected_observation && *ui.selected_observation < observations.size()) {
        const auto& selected = observations[*ui.selected_observation];
        if (selected.receiver_position && geographic_position_valid(*selected.receiver_position) &&
            ImGui::Button("Set area to selected position +/- 0.001 degrees")) {
            const auto& p = *selected.receiver_position;
            ui.survey_query.geographic_filter = true;
            ui.survey_query.south = std::max(-90.0, p.latitude - .001);
            ui.survey_query.north = std::min(90.0, p.latitude + .001);
            ui.survey_query.west = std::max(-180.0, p.longitude - .001);
            ui.survey_query.east = std::min(180.0, p.longitude + .001);
            ui.feedback(true, "Geographic bounds updated. Run analysis to apply the selection.");
        }
    }
    wrapped("Points use the last recorded receiver fix in each time bucket, not transmitter locations. Merged buckets may span movement. Missing fixes are omitted. Display mode changes neither recorded positions nor query/export results.");
    ImGui::PopID();
}

OccupancyScale frequency_summary(const Snapshot& snapshot, OccupancyScale scale = OccupancyScale::RevealLowActivity,
                                 FrequencyChartSelection* selection = nullptr) {
    label(snapshot.historical ? "RECORDED FREQUENCY SUMMARY" : "LIVE FREQUENCY SUMMARY");
    ImGui::Text("%zu frequency bins | %.3f measured seconds | %llu energy events",
        snapshot.frequencies.size(), snapshot.measurement_seconds, static_cast<unsigned long long>(snapshot.spectrum_events));
    if (snapshot.frequencies.empty()) {
        wrapped("Frequency measurements appear here after reception starts.");
        return scale;
    }
    wrapped("Full survey range. Busy time is time above the recorded activity threshold; frequency bins are measurement intervals, not mesh channels or packets.");
    int scale_choice = scale == OccupancyScale::Linear ? 1 : 0;
    ImGui::SetNextItemWidth(240);
    if (ImGui::Combo("Chart scale", &scale_choice, "Reveal low activity\0Linear 0-100%\0"))
        scale = scale_choice ? OccupancyScale::Linear : OccupancyScale::RevealLowActivity;
    wrapped(scale == OccupancyScale::Linear ? "Linear percentage scale." :
        "Nonlinear percentage scale expands brief activity while keeping 100% visible. Read the labeled ticks or hover for exact values.");
    wrapped("Columns show the maximum single-bin occupancy within each screen pixel, not combined channel occupancy. Zero has no activity bar; amber marks unobserved bins. Tiny positive values use a minimum-size marker.");
    ImGui::PushID("frequencySummaryChart");
    const double bin_width = snapshot.frequencies.front().width_hz;
    const double center = static_cast<double>(snapshot.config.center_hz);
    wrapped("Shading marks the receiver-center region where an internal artifact is possible. These are original, unfiltered measurements.");
    occupancy_chart(snapshot.frequencies, 210, scale, center - 2.5 * bin_width, center + 2.5 * bin_width, selection);
    ImGui::PopID();
    if (ImGui::BeginTable("frequencySummary", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                         ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit, {0, 170})) {
        for (const auto* heading : {"Lower MHz", "Upper MHz", "Width kHz", "Observed s", "Busy s", "Busy %", "Mean dBFS", "Peak dBFS"})
            ImGui::TableSetupColumn(heading);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(snapshot.frequencies.size()));
        while (clipper.Step()) for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const auto& bin = snapshot.frequencies[static_cast<size_t>(index)];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.6f", (static_cast<double>(bin.center_hz) - bin.width_hz / 2.0) / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.6f", (static_cast<double>(bin.center_hz) + bin.width_hz / 2.0) / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.3f", bin.width_hz / 1000.0);
            ImGui::TableNextColumn(); ImGui::Text("%.3f", bin.observed_seconds);
            ImGui::TableNextColumn(); ImGui::Text("%.3f", bin.active_seconds);
            ImGui::TableNextColumn();
            if (bin.observed_seconds > 0) ImGui::Text("%#.6g", ratio(bin.active_seconds, bin.observed_seconds) * 100);
            else ImGui::TextUnformatted("N/A");
            ImGui::TableNextColumn(); if (bin.observed_seconds > 0) ImGui::Text("%.1f", bin.mean_dbfs); else ImGui::TextUnformatted("N/A");
            ImGui::TableNextColumn(); if (bin.observed_seconds > 0) ImGui::Text("%.1f", bin.peak_dbfs); else ImGui::TextUnformatted("N/A");
        }
        ImGui::EndTable();
    }
    return scale;
}

void prepare_report_export(DesktopState& ui, const Snapshot& snapshot) {
    ui.export_options = ExportOptions{};
    ui.export_kind = 0;
    ui.export_session_id = snapshot.session_id;
    ui.export_from_analysis = ui.analysis_loaded && ui.analysis_session == snapshot.session_id;
    ui.export_query = ui.export_from_analysis ? ui.analyzed_query : SurveyQuery{};
    ui.export_time_bucket_seconds = 60;
    ui.export_geographic_cell_m = 100;
    ui.export_path.clear();
    ui.show_export = true;
}

ReportOptions selected_report_options(const DesktopState& ui) {
    ReportOptions options;
    // Detailed archive is a separate export path, not a ReportKind.
    switch (ui.export_kind) {
        case 0: options.kind = ReportKind::FrequencySummary; break;
        case 1: options.kind = ReportKind::TimeSummary; break;
        case 2: options.kind = ReportKind::GeographicSummary; break;
        case 3: options.kind = ReportKind::Waveforms; break;
        case 4: options.kind = ReportKind::ReceiverTrack; break;
        case 6: options.kind = ReportKind::Analysis; break;
        default: options.kind = ReportKind::FrequencySummary; break;
    }
    options.query = ui.export_query;
    options.query.time_bucket_seconds = ui.export_time_bucket_seconds;
    options.privacy = ui.export_options;
    options.geographic_cell_m = ui.export_geographic_cell_m;
    return options;
}

std::string report_export_block_reason(const DesktopState& ui, const Snapshot& snapshot, bool preview = false) {
    if (snapshot.running) return "Stop reception before exporting a consistent saved session.";
    if (!ui.export_session_id.empty() && ui.export_session_id != snapshot.session_id)
        return "The selected survey changed. Close and reopen this export window to review its selection.";
    if (snapshot.config.session_path.empty()) return "Open a saved survey or record a session before exporting.";
    if (ui.operation_busy()) return "A file operation is in progress.";
    if (ui.export_kind < 0 || ui.export_kind > 6) return "Choose a report type.";
    if (is_concentrator(snapshot.config) && ui.export_kind == 3)
        return "RAK records configured LoRa receptions, not software-discovered waveform observations.";
    if ((ui.export_kind == 2 || ui.export_kind == 4) && !ui.export_options.include_receiver_positions)
        return "This report requires Include receiver GPS coordinates.";
    if (!is_concentrator(snapshot.config) && (ui.export_kind == 1 || ui.export_kind == 6) && (!std::isfinite(ui.export_time_bucket_seconds) ||
        ui.export_time_bucket_seconds < .001 || ui.export_time_bucket_seconds > 1e10))
        return "Choose a time bucket from 0.001 through 10000000000 seconds.";
    if (!is_concentrator(snapshot.config) && (ui.export_kind == 2 || (ui.export_kind == 6 && ui.export_options.include_receiver_positions)) && (!std::isfinite(ui.export_geographic_cell_m) ||
        ui.export_geographic_cell_m < 10 || ui.export_geographic_cell_m > 10000))
        return "Choose geographic cells from 10 to 10000 meters.";
    if (preview && ui.export_kind == 6) return {};
    if (ui.export_path.empty()) return "Choose an export location.";
    auto extension = path_utf8(std::filesystem::path(std::u8string(ui.export_path.begin(), ui.export_path.end())).extension());
    for (auto& c : extension) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    if (ui.export_kind < 5 && extension != ".csv") return "Reports require a .csv filename.";
    if (ui.export_kind == 6 && extension != ".html") return "Analysis reports require a .html filename.";
    if (ui.export_kind == 5 && extension != ".csv" && extension != ".geojson")
        return "Detailed archives require a .csv or .geojson filename.";
    return {};
}

void start_analysis_report(Engine& engine, DesktopState& ui, const std::string& path,
                           std::function<void(const std::string&)> opener = open_local_report) {
    const auto options = selected_report_options(ui);
    ui.begin_operation("Generating analysis report...", [&engine,path,options,opener] {
        std::string error;
        if (!engine.export_report(path,options,error)) return DesktopState::OperationResult{false,error};
        try { opener(path); }
        catch (const std::exception& e) {
            return DesktopState::OperationResult{true,"Report saved at " + path + ". Browser opening failed: " + e.what()};
        }
        return DesktopState::OperationResult{true,"Report saved and sent to your browser: " + path};
    }, [&ui] {ui.show_export = false;});
}

std::string analysis_preview_path(const DesktopState& ui, const Snapshot& snapshot) {
    const auto directory = ui.preferences_ready ? ui.preference_locations.surveys_directory :
        path_utf8(std::filesystem::path(std::u8string(snapshot.config.session_path.begin(),snapshot.config.session_path.end())).parent_path());
    const auto generated = new_survey_path(directory);
    auto path = std::filesystem::path(std::u8string(generated.begin(),generated.end()));
    path = path.parent_path() / ("preview-" + path.stem().string() + ".html");
    return path_utf8(path);
}

void report_export_panel(Engine& engine, DesktopState& ui, const Snapshot& snapshot) {
    const bool rak = is_concentrator(snapshot.config);
    ImGui::SetNextItemWidth(-1);
    if (rak) {
        constexpr const char* names[] = {"Sampled frequency summary (CSV)","Scan readings over time (CSV)","Scan readings with positions (CSV)",
            "Unavailable waveform report","Receiver track (CSV)","Detailed archive (CSV / GeoJSON)","Analysis report (HTML)"};
        if (ui.export_kind < 0 || ui.export_kind > 6 || ui.export_kind == 3) ui.export_kind = 0;
        if (ImGui::BeginCombo("Report type",names[ui.export_kind])) {
            for (int i = 0; i < 7; ++i) if (i != 3 && ImGui::Selectable(names[i],ui.export_kind==i)) ui.export_kind=i;
            ImGui::EndCombo();
        }
        wrapped("RAK reports preserve sampled RSSI histograms. Sample fractions are not continuous occupancy, calibrated power or measured signal bandwidth.");
        if (ui.export_kind == 1 || ui.export_kind == 2)
            wrapped("One row per scan with its full host transaction interval. Positions are recorded receiver fixes; these rows are not resampled into time buckets or geographic cells.");
    } else {
        constexpr const char* names[] = {"Frequency summary (CSV)", "Time summary (CSV)", "Geographic summary (CSV)",
            "Waveform observations (CSV)", "Receiver track (CSV)", "Detailed archive (CSV / GeoJSON)", "Analysis report (HTML)"};
        if (ui.export_kind < 0 || ui.export_kind > 6 || (!desktop_lora_enabled && ui.export_kind == 3)) ui.export_kind = 0;
        if (ImGui::BeginCombo("Report type", names[ui.export_kind])) {
            for (int i = 0; i < 7; ++i)
                if ((desktop_lora_enabled || i != 3) && ImGui::Selectable(names[i], ui.export_kind == i)) ui.export_kind = i;
            ImGui::EndCombo();
        }
    }
    const bool archive = ui.export_kind == 5;
    const bool narrative = ui.export_kind == 6;
    if (narrative) wrapped(rak ? "A local report of sampled frequency activity, histogram evidence, optional receiver positions and limitations. Open in a browser and print to PDF." : "A readable local report of frequency activity, time patterns, optional receiver locations and measurement limits. Opens in a browser; print to PDF. No cloud service is used.");
    if (archive) {
        wrapped("Detailed archive: exports the whole saved session. Analysis frequency, time and geographic filters do not restrict this archive.", amber);
        wrapped(rak ? "The archive retains the recorded RSSI histograms and packet metadata. Individual RSSI sample order and IQ were never recorded." : "The archive contains the detail actually recorded; compact recordings cannot recover original 20 ms power samples.");
    } else {
        label("REPORT SELECTION");
        wrapped(ui.export_from_analysis ? "Using the latest completed analysis selection, captured when this export window opened."
            : "Using the full recorded frequency range and session time.");
        if (ui.export_query.lower_hz == 0 && ui.export_query.upper_hz == 0)
            ImGui::TextUnformatted("Frequency: full recorded range");
        else ImGui::Text("Frequency: %.6f - %.6f MHz", ui.export_query.lower_hz / 1e6, ui.export_query.upper_hz / 1e6);
        if (ui.export_query.elapsed_end == 0) ImGui::Text("Time: %.3f s through session end", ui.export_query.elapsed_start);
        else ImGui::Text("Time: %.3f - %.3f s", ui.export_query.elapsed_start, ui.export_query.elapsed_end);
        if (ui.export_query.geographic_filter) ImGui::TextWrapped("Receiver area: S %.6f N %.6f W %.6f E %.6f",
            ui.export_query.south, ui.export_query.north, ui.export_query.west, ui.export_query.east);
        else ImGui::TextUnformatted("Receiver area: no geographic filter");
        if (ImGui::Button("Use full session for report")) {
            ui.export_query = SurveyQuery{}; ui.export_from_analysis = false;
        }
        if (!rak && (ui.export_kind == 1 || narrative)) {
            ImGui::SetNextItemWidth(150);
            ImGui::InputDouble("Time bucket / seconds", &ui.export_time_bucket_seconds, 0, 0, "%.3f");
            wrapped("One summary per requested time bucket. Reports fail explicitly if the selected scope exceeds their row limit; they do not silently coarsen time.");
        }
        if (!rak && (ui.export_kind == 2 || (narrative && ui.export_options.include_receiver_positions))) {
            ImGui::SetNextItemWidth(150);
            ImGui::InputDouble("Geographic cell / meters", &ui.export_geographic_cell_m, 0, 0, "%.1f");
            wrapped("Geographic summaries group receiver observations into approximate meter cells. The default is 100 m. This grouping does not change original GPS fixes or imply transmitter locations.");
        } else if (ui.export_kind == 4) {
            wrapped("Receiver track exports recorded receiver fixes with the selected filters; it does not infer transmitter locations.");
        } else if (ui.export_kind == 3) {
            wrapped("Waveform observations retain inferred modem settings and evidence; they are not unique packets or confirmed transmitters.");
        }
    }
    ImGui::Separator();
    label("EXPORT PRIVACY");
    ImGui::Checkbox("Include antenna/receiver descriptions and survey notes", &ui.export_options.include_provenance);
    if (ui.export_options.include_provenance) wrapped("Free-form descriptions and notes may contain private location or operational details.", amber);
    ImGui::Checkbox("Include receiver GPS coordinates", &ui.export_options.include_receiver_positions);
    if (ui.export_options.include_receiver_positions) {
        int decimals = static_cast<int>(ui.export_options.coordinate_decimals);
        if (ImGui::SliderInt("Coordinate decimal places", &decimals, 0, 7)) ui.export_options.coordinate_decimals = static_cast<unsigned>(decimals);
        wrapped("Coordinate rounding affects this export only. It is not anonymization and is separate from geographic cell grouping.", amber);
        if (!rak && ui.export_kind == 2)
            wrapped("Rounded cell coordinates may coincide at low precision. Stable cell identifiers distinguish the groups; increase decimal places when finer exported coordinates are needed.");
    }
    label("EXPORT PREVIEW");
    ImGui::TextWrapped("Session: %s", snapshot.config.session_title.c_str());
    wrapped(archive ? "Whole-session archive. No raw IQ, undecoded frame bytes or ciphertext are included."
        : "Only the selected report category is exported. No raw IQ, undecoded frame bytes or ciphertext are included.");
    ImGui::Text("Descriptions/notes: %s | Receiver coordinates: %s",
        ui.export_options.include_provenance ? "included" : "excluded",
        ui.export_options.include_receiver_positions ? "included" : "excluded");
    ImGui::BeginDisabled(snapshot.running || snapshot.config.session_path.empty());
    if (ImGui::Button("Browse export location...")) begin_file_picker(ui, FilePickerPurpose::Export, ui.export_path, snapshot.config.session_path);
    ImGui::EndDisabled();
    selected_file_field("##exportPath", archive ? "Choose a new .csv or .geojson archive" : narrative ? "Choose a new .html analysis report" : "Choose a new .csv report", ui.export_path);
    if (narrative) {
        const auto preview_blocked = report_export_block_reason(ui, snapshot, true);
        ImGui::BeginDisabled(!preview_blocked.empty());
        if (ImGui::Button("Preview", {190, 0})) {
            try { start_analysis_report(engine, ui, analysis_preview_path(ui, snapshot)); }
            catch (const std::exception& e) { ui.feedback(false, e.what()); }
        }
        ImGui::EndDisabled();
        wrapped("Preview opens a local HTML copy using these selection and privacy options. No save location is needed. Preview copies remain in the app's default Surveys folder; delete them there when no longer needed.");
        if (!preview_blocked.empty()) wrapped(preview_blocked.c_str(), amber);
    }
    const auto blocked = report_export_block_reason(ui, snapshot);
    ImGui::BeginDisabled(!blocked.empty());
    if (ImGui::Button(archive ? "Write detailed archive" : "Write report", {190, 0})) {
        if (narrative) {
            start_analysis_report(engine, ui, ui.export_path);
        } else {
        std::string error;
        const bool ok = archive ? engine.export_session(ui.export_path, ui.export_options, error)
            : engine.export_report(ui.export_path, selected_report_options(ui), error);
        ui.feedback(ok, ok ? "Export written to the selected local path." : error);
        if (ok) ui.show_export = false;
        }
    }
    ImGui::EndDisabled();
    if (!blocked.empty() && !(narrative && blocked == "Choose an export location.")) wrapped(blocked.c_str(), amber);
}

void advanced_analysis_tab(Engine& engine, DesktopState& ui, const Snapshot& snapshot) {
    ImGui::BeginChild("analyzeScroll", {0, 0}, false);
    if (ui.analysis_loaded && ui.analysis_session != snapshot.session_id) {
        ui.analysis_loaded = false;
        ui.selected_observation.reset();
        ui.summary_selection = {}; ui.detail_selection = {};
    }
    const bool saved_available = !snapshot.config.session_path.empty() && !snapshot.session_id.empty();
    if (snapshot.running) {
        ImGui::TextColored(snapshot.recording ? accent : amber, "%s", snapshot.recording ? "RECORDING TO DISK" : "MEMORY ONLY - NOT RECORDING");
        if (!snapshot.recording) wrapped("Live measurements are visible below. This session has no saved history to analyze or export. Stop and start a new saved survey to retain future data.", amber);
    }
    if (saved_available) ImGui::TextWrapped("Survey file: %s", snapshot.config.session_path.c_str());
    else wrapped("Survey source: memory-only session. There is no saved file for these measurements.", amber);
    label("SELECT FREQUENCIES FOR THE TIME AND GPS PLOTS");
    wrapped("Drag across the frequency graph and release to analyze that interval. Click to center the chosen width. The blue outline marks the frequencies used by the plots below. This changes the analysis only.");
    ImGui::BeginDisabled(!saved_available || ui.analysis_busy());
    ImGui::PushID("graphWidth");
    ImGui::SetNextItemWidth(140);
    ImGui::Combo("Click width", &ui.width_choice, "62.5 kHz\0 125 kHz\0 250 kHz\0 500 kHz\0 Custom\0");
    if (ui.width_choice < 4) {
        constexpr double widths[] = {62.5, 125, 250, 500};
        ui.query_width_khz = widths[ui.width_choice];
    } else {
        ImGui::SetNextItemWidth(140); ImGui::InputDouble("Custom width / kHz", &ui.query_width_khz, 0, 0, "%.3f");
    }
    if (ImGui::Button("Analyze full frequency range")) select_analysis_frequencies(ui, snapshot, {});
    ImGui::PopID();
    ImGui::EndDisabled();
    const auto prepare_selection = [&](FrequencyChartSelection& selection) {
        selection.click_width_hz = ui.query_width_khz * 1000;
        selection.highlighted.reset();
        if (ui.analysis_loaded && !ui.analysis.bins.empty())
            selection.highlighted = FrequencyRange{ui.analysis.covered_lower_hz, ui.analysis.covered_upper_hz};
    };
    prepare_selection(ui.summary_selection);
    if (ui.analysis_loaded) wrapped(analysis_frequency_text(ui.analysis).c_str(), secondary);
    if (!saved_available) wrapped("Save a survey to enable frequency-linked time and GPS analysis. The live frequency graph remains available.", amber);
    if (ui.analysis_busy()) wrapped("Updating the linked plots from saved measurements...", amber);
    ui.occupancy_scale = frequency_summary(snapshot, ui.occupancy_scale,
        saved_available && !ui.analysis_busy() ? &ui.summary_selection : nullptr);
    if (saved_available && ui.summary_selection.requested) {
        select_analysis_frequencies(ui, snapshot, *ui.summary_selection.requested);
        ui.summary_selection.requested.reset();
    }
    ImGui::Separator();
    if (desktop_lora_enabled) {
        waveform_panel(snapshot, 160 * ui.ui_scale, ui.config.discover_lora,
            ui.analysis_loaded && ui.analysis_session == snapshot.session_id ? &ui.analysis : nullptr);
        ImGui::Separator();
    }
    wrapped("Use Open in the session toolbar to inspect a different saved survey.");
    ImGui::BeginDisabled(!saved_available);
    if (ImGui::Button("Reports / export...")) prepare_report_export(ui, snapshot);
    ImGui::EndDisabled();
    if (!saved_available) wrapped("Detailed analysis and export require a saved survey. The live frequency summary above updates without a saved file.", amber);
    help("Reports use the latest completed analysis selection, shown before export. Detailed archive is a separate whole-session option. Receiver positions and free-form notes are opt-in.");
    label("ANALYSIS SELECTION");
    wrapped("Chart selections update the linked plots automatically on release. For precise edges, elapsed intervals or receiver areas, edit these controls and press Run analysis. These are analysis views, not radio or modem presets.");
    if (ImGui::BeginTable("analysisControls", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Lower MHz");
        ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##analysisLowerMHz", &ui.query_lower_mhz, 0, 0, "%.6f");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Upper MHz");
        ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##analysisUpperMHz", &ui.query_upper_mhz, 0, 0, "%.6f");
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Combo("View width", &ui.width_choice, "62.5 kHz\0 125 kHz\0 250 kHz\0 500 kHz\0 Custom\0");
        ImGui::TableNextColumn();
        if (ui.width_choice < 4) {
            constexpr double widths[] = {62.5, 125, 250, 500};
            ui.query_width_khz = widths[ui.width_choice];
        }
        ImGui::BeginDisabled(ui.width_choice < 4);
        ImGui::SetNextItemWidth(115); ImGui::InputDouble("Width kHz", &ui.query_width_khz, 0, 0, "%.3f");
        ImGui::EndDisabled();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Button("Apply width from lower edge") && std::isfinite(ui.query_width_khz) && ui.query_width_khz > 0) {
            if (ui.query_lower_mhz == 0) ui.query_lower_mhz = (static_cast<double>(snapshot.config.center_hz) - snapshot.config.survey_span_hz / 2.0) / 1e6;
            ui.query_upper_mhz = ui.query_lower_mhz + ui.query_width_khz / 1000;
        }
        ImGui::TableNextColumn(); if (ImGui::Button("Full recorded frequency range")) ui.query_lower_mhz = ui.query_upper_mhz = 0;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Start elapsed / s");
        ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##analysisStartElapsed", &ui.survey_query.elapsed_start, 0, 0, "%.3f");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("End elapsed / s");
        ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##analysisEndElapsed", &ui.survey_query.elapsed_end, 0, 0, "%.3f");
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Requested time detail / s");
        ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##analysisTimeBucket", &ui.survey_query.time_bucket_seconds, 0, 0, "%.3f");
        ImGui::TableNextColumn(); if (ImGui::Button("Full recorded time")) ui.survey_query.elapsed_start = ui.survey_query.elapsed_end = 0;
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Frequency edges 0 / 0: full range. End time 0: through the latest saved observation.");
    ImGui::Checkbox("Filter by receiver geographic rectangle", &ui.survey_query.geographic_filter);
    if (ui.survey_query.geographic_filter && ImGui::BeginTable("geographicBounds", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("South latitude");
        ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##analysisSouth", &ui.survey_query.south, 0, 0, "%.6f");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("North latitude");
        ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##analysisNorth", &ui.survey_query.north, 0, 0, "%.6f");
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("West longitude");
        ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##analysisWest", &ui.survey_query.west, 0, 0, "%.6f");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("East longitude");
        ImGui::SetNextItemWidth(-1); ImGui::InputDouble("##analysisEast", &ui.survey_query.east, 0, 0, "%.6f");
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(!saved_available || ui.analysis_busy());
    if (ImGui::Button("Run analysis", {150, 0})) {
        queue_analysis(ui, snapshot, false);
    }
    ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::TextDisabled("Saved data only; no radio access or IQ replay.");
    if (!ui.analysis_loaded) {
        wrapped("Choose a saved survey and Run analysis for detailed time, frequency-width and location queries. The full-range frequency summary is above.");
        ImGui::EndChild();
        return;
    }
    const auto& result = ui.analysis;
    if (!result.detailed_available) {
        wrapped("Legacy session: time-resolved spectrum measurements and energy events were not recorded. Time, geographic and arbitrary-width occupancy analysis is unavailable; the original aggregate summary remains below.", amber);
        occupancy_chart(snapshot.frequencies, 200, ui.occupancy_scale);
        ImGui::EndChild();
        return;
    }
    label("MEASURED COVERAGE AND SELECTED-REGION OCCUPANCY");
    if (!result.bins.empty()) ImGui::Text("Analyzed frequencies: %.6f - %.6f MHz%s",
        result.covered_lower_hz / 1e6, result.covered_upper_hz / 1e6,
        ui.analyzed_query.lower_hz == 0 && ui.analyzed_query.upper_hz == 0 ? " (full recorded range)" : " (covered bin edges)");
    else ImGui::TextUnformatted("No recorded frequency bins intersect this selection.");
    ImGui::Text("Analyzed time: %.3f - %.3f s | spacing %.2f Hz",
        result.resolved_elapsed_start, result.resolved_elapsed_end, result.bin_width_hz);
    if (ui.analyzed_query.geographic_filter) ImGui::Text("Receiver area S %.6f N %.6f W %.6f E %.6f",
        ui.analyzed_query.south, ui.analyzed_query.north, ui.analyzed_query.west, ui.analyzed_query.east);
    if (result.observed_seconds > 0)
        ImGui::Text("Recorded any-bin busy: %.6f / %.6f observed s = %#.6g%% (includes receiver center)",
            result.busy_seconds, result.observed_seconds, ratio(result.busy_seconds, result.observed_seconds) * 100);
    else ImGui::TextColored(amber, "No observed time in this selection; occupancy is unavailable.");
    wrapped("Any-bin busy means at least one frequency was active. It is not the percentage of the bandwidth occupied or a packet count.");
    if (result.center_guard_bin_count) {
        ImGui::Checkbox("Exclude receiver-center region from time/GPS view", &ui.use_center_guard);
        if (result.mixed_acquisitions) ImGui::TextWrapped("Receiver-center guard follows each acquisition center (%zu selected bins across the acquisitions).", result.center_guard_bin_count);
        else ImGui::TextWrapped("Receiver-center guard: %.6f - %.6f MHz (%.3f kHz; %zu selected bins)",
            result.center_guard_lower_hz / 1e6, result.center_guard_upper_hz / 1e6,
            (result.center_guard_upper_hz - result.center_guard_lower_hz) / 1e3, result.center_guard_bin_count);
        if (result.observed_seconds > 0) ImGui::Text("Center-region busy: %#.6g%% of observed time",
            ratio(result.center_busy_seconds, result.observed_seconds) * 100);
        wrapped("This guard covers up to five bins around the receiver center, where an internal DC artifact may occur. It does not prove the signal is an artifact. Real signals may also be excluded; the guarded region remains unassessed. Original per-frequency measurements stay visible below.", amber);
        if (result.outside_center_bin_count && result.outside_center_observed_seconds > 0)
            ImGui::TextColored(accent, "Outside-center busy: %.6f / %.6f observed s = %#.6g%%",
                result.outside_center_busy_seconds, result.outside_center_observed_seconds, ratio(result.outside_center_busy_seconds, result.outside_center_observed_seconds) * 100);
        else ImGui::TextColored(amber, "Outside-center busy: unavailable (no unguarded observed frequencies).");
    }
    wrapped(guarded_view(ui) ? "Time/GPS view uses OUTSIDE-CENTER busy time. Clear the checkbox to compare the original all-bin result." :
        "Time/GPS view uses the original busy time across ALL selected frequencies.");
    ImGui::Text("%llu measurement tiles | %llu intersecting energy events | %zu coverage gaps",
        static_cast<unsigned long long>(result.tile_count), static_cast<unsigned long long>(result.event_count), result.gaps.size());
    ImGui::TextWrapped("Quality: %s", quality_text(result.quality).c_str());
    quality_help(result.quality);
    if (ImGui::CollapsingHeader("What do these quality flags mean?"))
        wrapped(quality_explanation(result.quality).c_str());
    if (result.observed_seconds > 0) {
        const double located = std::max(0.0, result.observed_seconds - result.missing_end_position_seconds);
        ImGui::TextWrapped("Receiver-position coverage: %.3f / %.3f observed s (%.2f%%) have a stored endpoint position",
            located, result.observed_seconds, 100 * located / result.observed_seconds);
        ImGui::TextWrapped("Missing tile-start positions: %.3f s | Missing tile-end positions: %.3f s",
            result.missing_start_position_seconds, result.missing_end_position_seconds);
        wrapped("These durations describe the selected saved measurements. Start/end missing times can overlap. Position coverage is not GPS accuracy; missing positions are separate from RF acquisition gaps.");
    }
    wrapped("Frequency edges include complete recorded bins that intersect the selection. dBFS is uncalibrated; these results do not identify emitters or certify a channel.");
    wrapped("Per-frequency chart: original measurements, including the shaded receiver-center guard.");
    wrapped("Drag or click this frequency graph to narrow the linked time/GPS view. Use Analyze full frequency range to widen it again.");
    prepare_selection(ui.detail_selection);
    occupancy_chart(result.bins, 205, ui.occupancy_scale, result.center_guard_lower_hz, result.center_guard_upper_hz,
        ui.analysis_busy() ? nullptr : &ui.detail_selection);
    if (ui.detail_selection.requested) {
        select_analysis_frequencies(ui, snapshot, *ui.detail_selection.requested);
        ui.detail_selection.requested.reset();
    }
    ImGui::TextWrapped("Full-interval overview: %zu time buckets | displayed detail %.6g s | requested detail %.6g s",
        result.observations.size(), result.effective_time_bucket_seconds, ui.analyzed_query.time_bucket_seconds);
    if (result.observations_coarsened)
        wrapped("Time buckets were combined to cover the entire selected interval within the display budget. All observed time contributes. Select a shorter interval for finer time and GPS detail.", amber);
    if (result.observations_truncated)
        wrapped("Observation plotting is disabled by the display budget; aggregate results still include the full selection.", amber);
    if (ui.reveal_time_plot) { ImGui::SetScrollHereY(0.1f); ui.reveal_time_plot = false; }
    ImGui::BeginDisabled(!saved_available || ui.analysis_busy());
    if (ImGui::Button("Show time/GPS for full frequency range")) select_analysis_frequencies(ui, snapshot, {});
    ImGui::EndDisabled();
    busy_time_chart(ui, 185);
    geographic_rf_view(ui, 240);
    burst_table(result.bursts,result.burst_count,230);
    if (ImGui::CollapsingHeader("Show raw energy fragments")) energy_event_table(result.events,230);
    if (result.events_truncated) wrapped("Only the bounded event view is shown. Narrow the selection to inspect more events.", amber);
    if (ImGui::CollapsingHeader("Recorded coverage gaps", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ui.analyzed_query.geographic_filter)
            wrapped("Coverage gaps have no receiver position and are omitted from geographic selections. Their applicability to this area is unknown.", amber);
        else if (result.gaps.empty()) wrapped("No explicit application coverage gap records in this selection. Unknown upstream loss may remain.");
        for (const auto& gap : result.gaps) {
            if (gap.reason == "source_stall" && gap.missing_samples == 0)
                ImGui::TextWrapped("%.6f - %.6f s | missing sample count unknown | %s", gap.elapsed_start_seconds, gap.elapsed_end_seconds, gap.reason.c_str());
            else ImGui::TextWrapped("%.6f - %.6f s | %llu missing samples | %s",
                gap.elapsed_start_seconds, gap.elapsed_end_seconds, static_cast<unsigned long long>(gap.missing_samples), gap.reason.c_str());
        }
    }
    ImGui::EndChild();
}

void dialogs(Engine& engine, DesktopState& ui, const Snapshot& snapshot) {
    if (!desktop_lora_enabled) { ui.show_keys = false; ui.show_detail = false; }
    if (ui.show_keys) {
        ImGui::SetNextWindowSize({650, 590}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Authorized channel keys", &ui.show_keys, ImGuiWindowFlags_NoCollapse)) {
            wrapped("The public Meshtastic key is configured separately from your private key records. Private keys stay in this process and are never saved.");
            ImGui::BeginDisabled(snapshot.running || snapshot.historical);
            if (ImGui::Checkbox("Use Meshtastic public default key (AQ==)", &ui.public_meshtastic_key_enabled)) {
                ui.apply_public_key(engine); ui.persist_preferences();
            }
            ImGui::EndDisabled();
            wrapped("One public key covers all presets and channel names that use it. It does not add receive profiles or bypass frame integrity checks.");
            wrapped("These records are independent of receive frequencies and profiles. Every live frame is evaluated against the configured keyring.");
            const auto records = engine.key_records();
            ui.selected_key_record = std::clamp(ui.selected_key_record, 0, static_cast<int>(records.size()) - 1);
            const auto& selected_record = records[static_cast<size_t>(ui.selected_key_record)];
            const std::string selected_title = std::to_string(ui.selected_key_record + 1) + " / " +
                (selected_record.configured ? selected_record.label : "empty");
            ImGui::BeginDisabled(snapshot.running);
            if (ImGui::BeginCombo("Key record", selected_title.c_str())) {
                for (const auto& record : records) {
                    const std::string title = std::to_string(record.slot + 1) + " / " +
                        (record.configured ? record.label + (record.restrict_channel_name ? " / named channel" : " / key only") : "empty");
                    if (ImGui::Selectable(title.c_str(), record.slot == static_cast<size_t>(ui.selected_key_record))) {
                        ui.selected_key_record = static_cast<int>(record.slot);
                        ui.key_mode = record.configured && record.restrict_channel_name ? 0 : 1;
                        if (!record.channel_name.empty()) copy_text(ui.channel_name, record.channel_name);
                        copy_text(ui.key_label, record.label.empty() ? "Survey key" : record.label);
                        erase_secret(ui.key_input);
                        ui.authorize_keys = false;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::Combo("Key scope", &ui.key_mode, "Named channel\0Key only / any channel name\0")) {
                erase_secret(ui.key_input);
                ui.authorize_keys = false;
            }
            if (ui.key_mode == 0) {
                ImGui::InputText("Channel name", ui.channel_name.data(), ui.channel_name.size());
                help("Use the exact, case-sensitive resolved channel name, up to 32 bytes. This narrows key attempts by the over-the-air channel hash across every receive profile.");
            } else {
                ImGui::InputText("Record label", ui.key_label.data(), ui.key_label.size());
                wrapped("Key-only scope permits this key regardless of channel name or header hash. Automatic discovery and optional manual profiles share this keyring; frame integrity checks still apply.", amber);
            }
            ImGui::InputText("Key / hex or Base64", ui.key_input.data(), ui.key_input.size(),
                ImGuiInputTextFlags_Password | ImGuiInputTextFlags_NoUndoRedo);
            help("Enter a 16- or 32-byte key as hexadecimal or padded standard Base64. AQ== explicitly selects Meshtastic's public default key. Other one-byte shorthand values are not supported.");
            ImGui::Checkbox("I am authorized to use this key for the selected scope", &ui.authorize_keys);
            ImGui::BeginDisabled(!ui.authorize_keys || ui.key_input[0] == 0 || (ui.key_mode == 0 ? ui.channel_name[0] == 0 : ui.key_label[0] == 0));
            if (ImGui::Button("Set / replace selected key record")) {
                std::string error;
                std::string transient_key(ui.key_input.data());
                const bool ok = ui.key_mode == 0
                    ? engine.set_channel_key(static_cast<size_t>(ui.selected_key_record), ui.channel_name.data(), transient_key, error)
                    : engine.set_survey_key(static_cast<size_t>(ui.selected_key_record), ui.key_label.data(), transient_key, error);
                erase_secret(transient_key);
                erase_secret(ui.key_input);
                ui.authorize_keys = false;
                ui.feedback(ok, ok ? "Authorized key record installed in memory. Input buffer cleared." : error);
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::Spacing();
            ImGui::BeginDisabled(snapshot.running);
            if (ImGui::Button("Clear all in-memory keys")) {
                engine.clear_keys();
                ui.public_meshtastic_key_enabled = false; ui.persist_preferences();
                erase_secret(ui.key_input);
                ui.feedback(true, "Configured keys cleared and public-key default disabled. Original saved survey files are unchanged.");
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            wrapped("Successful channel decryption and schema validation do not authenticate a Meshtastic sender. Recipient private-key messages and MeshCore keys are outside the currently enabled decoder.");
            wrapped("Duplicate eligible key material is tried once. Distinct keys that produce competing plausible envelopes suppress classification evidence as ambiguous; acceptance does not authenticate the sender.");
            wrapped("Key entry and decoding use process memory. The application does not claim control over operating-system swap or crash handling.");
        }
        ImGui::End();
    }
    if (!ui.show_keys && ui.key_input[0] != 0) erase_secret(ui.key_input);
    if (ui.show_licenses) {
        ImGui::SetNextWindowSize({820, 650}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("About & licenses", &ui.show_licenses, ImGuiWindowFlags_NoCollapse)) {
            ImGui::Text("OVMeshDR++ %s", Engine::version().c_str());
            wrapped("Copyright (C) 2026 OVMeshDRpp contributors. Third-party copyrights are retained below.");
            wrapped("Free software under GNU GPL version 3. You may redistribute and modify it under the license terms. WITHOUT ANY WARRANTY.");
            wrapped("SDR++ inspired the interface. Selected SDRangel receiver logic is adapted with attribution. This is an independent project.");
            const auto notices = license_notices();
            ui.selected_license = std::clamp(ui.selected_license, 0, int(notices.size()) - 1);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##licenseDocument", notices[size_t(ui.selected_license)].name.data())) {
                for (size_t i = 0; i < notices.size(); ++i)
                    if (ImGui::Selectable(notices[i].name.data(), ui.selected_license == int(i))) ui.selected_license = int(i);
                ImGui::EndCombo();
            }
            ImGui::Separator();
            if (ImGui::BeginChild("licenseText", {0, -ImGui::GetFrameHeightWithSpacing()}, ImGuiChildFlags_Borders)) {
                ImGui::PushTextWrapPos(0);
                const auto text = notices[size_t(ui.selected_license)].text;
                ImGui::TextUnformatted(text.data(), text.data() + text.size());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
            if (ImGui::Button("Close")) ui.show_licenses = false;
        }
        ImGui::End();
    }
    if (ui.show_detail && ui.selected) {
        ImGui::SetNextWindowSize({710, 530}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Reception details", &ui.show_detail)) reception_detail(*ui.selected);
        ImGui::End();
    }
    if (ui.show_export) {
        ImGui::SetNextWindowSize({760, 720}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Export survey / preview", &ui.show_export, ImGuiWindowFlags_NoCollapse)) {
            report_export_panel(engine, ui, snapshot);
        }
        ImGui::End();
    }
}

void receiver_diagnostics(const Snapshot& snapshot, bool show_by_default) {
    if (snapshot.historical || !ImGui::CollapsingHeader("Receiver diagnostics",
            show_by_default ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None)) return;
    wrapped("Live acquisition counters for this receiver.");
    if (desktop_lora_enabled) {
    if (snapshot.lane_health.empty()) ImGui::TextDisabled("No active receiver profiles.");
    for (size_t i = 0; i < snapshot.lane_health.size(); ++i) {
        const auto& lane = snapshot.lane_health[i];
        const auto& p = lane.phy;
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextColored(secondary, "Profile %zu  |  %.6f MHz  |  %.1f s processed  |  CRC failures %llu",
            i + 1, static_cast<double>(lane.frequency_hz) / 1e6, lane.processed_seconds,
            static_cast<unsigned long long>(lane.crc_failures));
        ImGui::TextWrapped("Preambles %llu  |  Sync matches %llu / rejects %llu  |  Headers valid %llu / failed %llu  |  Completed %llu",
            static_cast<unsigned long long>(p.preamble_candidates), static_cast<unsigned long long>(p.sync_matches),
            static_cast<unsigned long long>(p.sync_rejections), static_cast<unsigned long long>(p.headers_valid),
            static_cast<unsigned long long>(p.headers_failed), static_cast<unsigned long long>(p.completed_frames));
        ImGui::TextWrapped("Sync rejection reasons: low ratio %llu  |  timeout %llu  |  first mismatch %llu  |  second mismatch %llu",
            static_cast<unsigned long long>(p.sync_low_ratio), static_cast<unsigned long long>(p.sync_timeout),
            static_cast<unsigned long long>(p.sync_first_mismatch), static_cast<unsigned long long>(p.sync_second_mismatch));
        ImGui::PopID();
    }
    }
    ImGui::TextDisabled("Dropped application samples %llu  |  Upstream loss %s",
        static_cast<unsigned long long>(snapshot.dropped_samples), snapshot.upstream_loss_unknown ? "unknown" : "known synthetic source");
}

#include "ui_workflow.hpp"
#include "ui_capture.hpp"


void report_receiver_stop(const Snapshot& final, bool expired) {
        if (!final.config.synthetic && final.config.hardware_receiver == HardwareReceiver::Rak5146) {
            std::printf("Desktop RAK receive stopped timed_out=%u error=%u elapsed_seconds=%.3f scans=%llu rssi_samples=%llu receptions=%llu classified_receptions=%llu\n",
                expired ? 1u : 0u, final.error.empty() ? 0u : 1u, final.elapsed_seconds,
                static_cast<unsigned long long>(final.concentrator_scans), static_cast<unsigned long long>(final.concentrator_rssi_samples),
                static_cast<unsigned long long>(final.total_receptions), static_cast<unsigned long long>(final.classified_receptions));
            for (size_t i = 0; i < final.concentrator_health.size(); ++i) {
                const auto& h = final.concentrator_health[i];
                std::printf("Desktop final board=%zu scans=%llu rssi_samples=%llu receptions=%llu crc_failures=%llu\n", i + 1,
                    static_cast<unsigned long long>(h.scans), static_cast<unsigned long long>(h.rssi_samples),
                    static_cast<unsigned long long>(h.receptions), static_cast<unsigned long long>(h.crc_failures));
            }
            std::fflush(stdout);
            return;
        }
        std::printf("Desktop spectrum groups=%llu raw_fragments=%llu\n",
            static_cast<unsigned long long>(final.spectrum_bursts),static_cast<unsigned long long>(final.spectrum_events));
        std::printf("Desktop receive stopped timed_out=%u error=%u elapsed_seconds=%.3f input_seconds=%.3f measurement_seconds=%.3f delivered_samples=%llu dropped_samples=%llu upstream_loss_unknown=%u receptions=%llu classified_receptions=%llu\n",
            expired ? 1u : 0u, final.error.empty() ? 0u : 1u, final.elapsed_seconds, final.input_seconds, final.measurement_seconds,
            static_cast<unsigned long long>(final.delivered_samples), static_cast<unsigned long long>(final.dropped_samples),
            final.upstream_loss_unknown ? 1u : 0u, static_cast<unsigned long long>(final.total_receptions),
            static_cast<unsigned long long>(final.classified_receptions));
        for (const auto& band : final.discovery.bands) {
            if (!band.runtime_diagnostics_available) continue;
            std::printf("Desktop final discovery_scope=latest_acquisition subband=%u center_hz=%.3f processed_output_samples=%llu abandoned_output_samples=%llu source_gap_input_samples=%llu fft_searches=%llu windows=%llu resets_after_gap=%llu candidate_limit_hits=%llu track_limit_hits=%llu\n",
                band.subband_index, band.center_hz,
                static_cast<unsigned long long>(band.processed_samples), static_cast<unsigned long long>(band.abandoned_samples),
                static_cast<unsigned long long>(band.source_gap_input_samples), static_cast<unsigned long long>(band.fft_searches),
                static_cast<unsigned long long>(band.windows), static_cast<unsigned long long>(band.resets_after_gap),
                static_cast<unsigned long long>(band.candidate_limit_hits), static_cast<unsigned long long>(band.track_limit_hits));
        }
        for (size_t i = 0; i < final.lane_health.size(); ++i) {
            const auto& h = final.lane_health[i]; const auto& p = h.phy;
            std::printf("Desktop final lane=%zu frequency_hz=%llu processed_seconds=%.3f frames=%llu classified=%llu crc_failures=%llu resets=%llu preamble_candidates=%llu sync_matches=%llu sync_rejections=%llu sync_low_ratio=%llu sync_timeout=%llu sync_first_mismatch=%llu sync_second_mismatch=%llu headers_valid=%llu headers_failed=%llu completed_frames=%llu\n",
                i + 1, static_cast<unsigned long long>(h.frequency_hz), h.processed_seconds,
                static_cast<unsigned long long>(h.frames), static_cast<unsigned long long>(h.classified),
                static_cast<unsigned long long>(h.crc_failures), static_cast<unsigned long long>(h.resets),
                static_cast<unsigned long long>(p.preamble_candidates), static_cast<unsigned long long>(p.sync_matches),
                static_cast<unsigned long long>(p.sync_rejections), static_cast<unsigned long long>(p.sync_low_ratio),
                static_cast<unsigned long long>(p.sync_timeout), static_cast<unsigned long long>(p.sync_first_mismatch),
                static_cast<unsigned long long>(p.sync_second_mismatch), static_cast<unsigned long long>(p.headers_valid),
                static_cast<unsigned long long>(p.headers_failed), static_cast<unsigned long long>(p.completed_frames));
        }
        std::fflush(stdout);
}

} // namespace

int run_desktop(Engine& engine, int maximum_frames, bool auto_demo,
                const ReceiverConfig* launch_config, double duration_seconds, bool prepare_only, bool until_stopped,
                const std::string& settings_directory) {
    // A supplied synthetic configuration can merely prefill ordinary desktop
    // controls. Only explicit hardware/setup or until-stopped launches own a
    // managed receive lifecycle and its source/deadline requirements.
    const bool managed_launch = prepare_only || until_stopped || (launch_config && !launch_config->synthetic);
    if ((managed_launch && !launch_config) ||
        (managed_launch && (launch_config->synthetic != auto_demo || (prepare_only && auto_demo) ||
        (!until_stopped && (!std::isfinite(duration_seconds) || duration_seconds <= 0 || duration_seconds > 86400))))) {
        std::fputs("Managed desktop reception requires a matching source configuration and either --until-stopped or 0 < seconds <= 86400.\n", stderr);
        return 2;
    }
    DesktopStopSignal stop_signal(until_stopped);
    // Initializing the windowing system is not receiver enumeration. Engine
    // construction and every pre-consent snapshot must likewise remain passive.
    if (!glfwInit()) {
        std::fputs("Unable to initialize the local desktop window system.\n", stderr);
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif
    GLFWwindow* window = glfwCreateWindow(1460, 940, "OVMeshDR++ | Local RF survey", nullptr, nullptr);
    if (!window) {
        std::fputs("Unable to create the local OpenGL desktop window.\n", stderr);
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(window, 1000, 680, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    setup_style();
    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 150")) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    DesktopState ui;
    const auto fonts = system_font_candidates();
    const auto load_font = [&](const std::vector<std::string>& candidates) -> ImFont* {
        for (const auto& path : candidates)
            if (auto* font = io.Fonts->AddFontFromFileTTF(path.c_str(), 14.f)) return font;
        return io.Fonts->AddFontDefault();
    };
    ui.sans_font = load_font(fonts.ui_sans);
    ui.mono_font = load_font(fonts.monospace);
    io.FontDefault = ui.sans_font;
    ui.passive_smoke = maximum_frames > 0 && !auto_demo;
    std::atomic<bool> timed_out{false};
    std::unique_ptr<ReceiveDeadline> watchdog;
    int result = 0;
    bool stop_reported = false;
    ui.manual_stop = until_stopped;
    if (launch_config) {
        ui.config = *launch_config;
        ui.source = receiver_source_index(*launch_config);
        ui.use_launch_detection(*launch_config);
        ui.prepared_run = prepare_only;
        ui.passive_smoke = maximum_frames > 0 && !auto_demo;
        ui.timed_duration = duration_seconds;
        ui.save_session = !ui.config.session_path.empty();
        ui.session_path = ui.config.session_path;
        copy_text(ui.session_title, ui.config.session_title);
        copy_text(ui.device_serial, ui.config.device_serial);
        copy_text(ui.antenna_description, ui.config.antenna_description);
        copy_text(ui.receiver_description, ui.config.receiver_description);
        copy_text(ui.survey_notes, ui.config.survey_notes);
        if (!managed_launch) {
            // Ordinary desktop/demo setup accepts the same supplied receiver,
            // recording and provenance fields without arming a managed timer.
            if (auto_demo) ui.start(engine, false);
        } else {
        // Both launch paths use the same successful-start callback. Timed runs
        // arm an independent watchdog; operator-controlled runs have no deadline.
        ui.after_start = [&] {
            if (!until_stopped) {
                ui.deadline = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(duration_seconds));
                watchdog = std::make_unique<ReceiveDeadline>(engine, timed_out, ui.deadline);
                ui.timed_run = true;
            }
            ui.managed_started = true;
            const double utc_deadline = until_stopped ? 0 : std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() + duration_seconds;
#ifdef _WIN32
            const auto process_id = ::_getpid();
#else
            const auto process_id = ::getpid();
#endif
            std::printf("Desktop control pid=%ld until_stopped=%u recording=%u stop_signal=%s window_stays_open_after_stop=%u\n",
                static_cast<long>(process_id), until_stopped ? 1u : 0u, ui.save_session ? 1u : 0u, until_stopped && external_stop_supported ? "SIGINT" : "none", until_stopped ? 1u : 0u);
            const auto applied = engine.snapshot().config;
            const auto acquisition_log = desktop_acquisition_log(applied, engine.configured_key_count(),
                until_stopped ? 0 : duration_seconds, utc_deadline);
            std::fputs(acquisition_log.c_str(), stdout);
            std::fflush(stdout);
        };
        if (prepare_only) {
            ui.feedback(true, "Setup only. Review receiver and survey settings, then start reception. The timer has not started.");
            std::printf("Desktop setup ready device_opened=0 timer_armed=0 duration_seconds=%.3f passive_smoke=%u\n",
                duration_seconds, ui.passive_smoke ? 1u : 0u);
            std::fflush(stdout);
        } else {
            // main() has checked the explicit automatic-start consent flag.
            ui.start(engine, !ui.config.synthetic);
            if (!ui.managed_started) {
                std::fprintf(stderr, "Desktop receiver could not start: %s\n", ui.notice.c_str());
                result = 1;
            }
        }
        }
    } else if (auto_demo) ui.start(engine, false);
    const auto initial = engine.snapshot();
    if (!managed_launch && !auto_demo && maximum_frames == 0 && !initial.historical) {
        ui.initialize_preferences(settings_directory, true, !launch_config);
        ui.apply_public_key(engine);
        if (launch_config) {
            // Explicit command-line acquisition choices win over preferences.
            ui.config = *launch_config;
            ui.use_launch_detection(*launch_config);
        }
        // Explicit launch recording destinations take precedence for this run.
        if (launch_config && !launch_config->session_path.empty()) {
            ui.save_session = true;
            ui.session_path = launch_config->session_path;
        }
        if (!receiver_source_available(ui.source, initial)) {
            const std::string unavailable_name = receiver_source_name(ui.config);
            ui.select_receiver(0);
            ui.feedback(false, unavailable_name + " support is unavailable in this build. Synthetic source selected.");
        } else if (ui.source != 0 && ui.gps_enabled && !ui.passive_smoke) {
            ui.connect_selected_gps(engine);
        }
    }
    if (initial.historical) {
        // Opening a saved file is a passive initial view. Its acquisition
        // metadata populates controls, but no source or receive timer starts.
        ui.config = initial.config;
        ui.config.session_path.clear();
        ui.source = receiver_source_index(initial.config);
        ui.save_session = false;
        ui.passive_smoke = maximum_frames > 0;
        copy_text(ui.session_title, initial.config.session_title);
        copy_text(ui.device_serial, initial.config.device_serial);
        copy_text(ui.antenna_description, initial.config.antenna_description);
        copy_text(ui.receiver_description, initial.config.receiver_description);
        copy_text(ui.survey_notes, initial.config.survey_notes);
        ui.focus_analysis = true;
        queue_analysis(ui, initial, false);
        std::printf("Saved survey opened read-only. No radio or GPS access.\n");
        std::fflush(stdout);
    }
    int frame = 0;
    while (result == 0 && !timed_out.load() && (maximum_frames <= 0 || frame < maximum_frames)) {
        glfwPollEvents();
        ui.finish_operation();
        progress_analysis(engine, ui);
        if (ui.close_approved) break;
        if (glfwWindowShouldClose(window)) {
            if (managed_launch || maximum_frames > 0) break;
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            const auto closing = engine.snapshot();
            if (ui.operation_busy()) ui.feedback(false, "Wait for the current file operation before closing.");
            else if ((has_session_data(closing) || closing.running) && closing.config.session_path.empty()) ui.close_requested = true;
            else if (closing.historical || !has_session_data(closing)) break;
            else ui.begin_operation("Saving before closing...", [&engine] {
                engine.stop(); std::string error; const bool ok = engine.save_session(error);
                return DesktopState::OperationResult{ok, ok ? "Session saved." : error};
            }, [&ui] { ui.close_approved = true; });
        }
        // Check receiver failure before the minimized-window fast path as well.
        // For timed runs, the watchdog stops hardware even if rendering stalls.
        if (until_stopped && desktop_stop_requested && ui.managed_started && !stop_reported) {
            engine.stop();
            ui.feedback(true, "Reception stopped on request. Retained results remain open for review.");
        }
        auto snapshot = engine.snapshot();
        if (until_stopped && ui.managed_started && !stop_reported && !snapshot.error.empty()) {
            engine.stop();
            snapshot = engine.snapshot();
            ui.feedback(false, "Receiver stopped after an error. Results remain open for inspection.");
        }
        if (until_stopped && ui.managed_started && !snapshot.running && !stop_reported) {
            report_receiver_stop(snapshot, false);
            stop_reported = true;
        }
        if (ui.timed_run && !snapshot.error.empty()) { result = 1; break; }
        if (timed_out.load()) break;
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            glfwWaitEventsTimeout(0.1);
            if (maximum_frames > 0) ++frame;
            continue;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        render(engine, ui, snapshot);
        ImGui::Render();
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.045f, 0.058f, 0.075f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        capture_waveform_frame(window, engine, ui, snapshot);
        glfwSwapBuffers(window);
        progress_analysis(engine, ui);
        ++frame;
    }
    if (watchdog) watchdog->cancel_and_join();
    if (ui.operation.valid()) { ui.operation.wait(); ui.operation.get(); }
    if (ui.analysis_operation.valid()) { ui.analysis_operation.wait(); ui.analysis_operation.get(); }
    engine.stop();
    if (launch_config && ui.managed_started && !stop_reported)
        report_receiver_stop(engine.snapshot(), timed_out.load());
    if (!engine.snapshot().error.empty()) result = 1;
    engine.disconnect_gps();
    engine.clear_keys();
    erase_secret(ui.key_input);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return result;
}

} // namespace ovmesh
