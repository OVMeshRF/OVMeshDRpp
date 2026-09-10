// SPDX-License-Identifier: GPL-3.0-or-later
// Actual ImGui rendering with generated metadata only: no window, RF, GPS,
// device enumeration, clipboard, network or ordinary desktop profile access.
#include "../src/ui.cpp"
#include <imgui_internal.h>
#include <iostream>
#include <string_view>

namespace {
using namespace ovmesh;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void contains(const std::string& text, const char* expected) {
    if (text.find(expected) == std::string::npos)
        throw std::runtime_error(std::string("Missing rendered text: ") + expected + "\n" + text);
}

class Canvas {
public:
    Canvas() {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.DisplaySize = {1600, 1100}; io.DeltaTime = 1.0f / 60;
        unsigned char* pixels; int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        require(pixels && width > 0 && height > 0, "Build only an in-memory font atlas");
    }
    ~Canvas() { ImGui::DestroyContext(); }
    template<class Render> std::string frame(Render render, bool capture = true) {
        ImGui::NewFrame(); ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Waveform fixture", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
        if (capture) ImGui::LogToBuffer();
        render();
        std::string text;
        if (capture) { text = ImGui::GetCurrentContext()->LogBuffer.c_str(); ImGui::LogFinish(); }
        ImGui::End(); ImGui::Render();
        const auto* context = ImGui::GetCurrentContext();
        require(context->CurrentWindowStack.empty() && context->BeginPopupStack.empty() && !context->CurrentTable,
            "Waveform UI balances window, popup and table scopes");
        const auto* draw = ImGui::GetDrawData();
        require(draw && draw->Valid && draw->TotalVtxCount > 0, "Waveform UI produces rendered geometry");
        for (int list = 0; list < draw->CmdListsCount; ++list)
            for (const auto& vertex : draw->CmdLists[list]->VtxBuffer)
                require(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y), "Waveform UI geometry is finite");
        return text;
    }
};

void open_details() {
    ImGui::GetStateStorage()->SetBool(ImGui::GetID("Waveform interpretation and discovery counters"), true);
}

Snapshot observations_fixture() {
    Snapshot s; s.session_id = "source-generated-waveforms"; s.running = true;
    s.config.center_hz = 865750000; s.config.survey_span_hz = 4500000;
    s.config.lanes.clear(); s.config.discover_lora = true;
    s.discovery.enabled = true; s.discovery.observations = 2;
    s.discovery.accepted_input_samples = 16000000; s.discovery.channelized_input_samples = 16000000;
    WaveformObservation first; first.id = 45; first.center_hz = 864123456.25;
    first.bandwidth_hz = 250000; first.spreading_factor = 11;
    first.first_observed_elapsed = 1.0000125; first.delimiter_elapsed = 1.071234125;
    first.delimiter_utc = 1700000000.125;
    first.up_match = .88; first.down_match = .91; first.contributing_subbands = 2;
    first.complete_in_requested_range = true;
    WaveformObservation second = first; second.id = 51; second.center_hz = 867321987.75;
    second.bandwidth_hz = 500000; second.spreading_factor = 7;
    second.first_observed_elapsed = 3.10; second.delimiter_elapsed = 3.103456;
    second.delimiter_utc = 1700086400.875;
    second.complete_in_requested_range = false; second.association_ambiguous = true;
    s.waveforms = {first, second};
    // Unrelated energy width must never be promoted to a modem setting.
    SpectrumBurst energy; energy.lower_hz = 864001000; energy.upper_hz = 864017000;
    s.recent_spectrum_bursts = {energy}; s.spectrum_bursts = 1000;
    s.measurement_seconds = 17.25; s.delivered_samples = 276000000;
    return s;
}

void reception_timestamps(Canvas& canvas) {
    require(timestamp_text(1700000000.125) == "2023-11-14 22:13:20.125", "Full UTC date and milliseconds");
    require(timestamp_text(1700000000.999) == "2023-11-14 22:13:20.999", "Millisecond boundary is preserved");
    require(timestamp_text(1700006400.0) == "2023-11-15 00:00:00.000", "UTC midnight includes the new date");
    for (double value : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity(), std::numeric_limits<double>::max(), 253402300800.0})
        require(timestamp_text(value) == "Unavailable", "Missing/invalid timestamp is not replaced by current time");

    DesktopState ui; auto s = observations_fixture();
    s.running = false; s.historical = true;
    Reception record; record.id = 1; record.utc_seconds = 1700000000.125; record.elapsed_seconds = 5.25;
    record.frequency_hz = 864123456; record.bandwidth_hz = 250000; record.spreading_factor = 11;
    s.receptions = {record};
    const auto log = canvas.frame([&] { packet_table(ui, s, 150); });
    contains(log, "Received at (UTC)"); contains(log, "2023-11-14 22:13:20.125");
    const auto detail = canvas.frame([&] { content_detail(record); });
    contains(detail, "2023-11-14 22:13:20.125 UTC"); contains(detail, "Elapsed session time: 5.250000 s");
    const auto signals = canvas.frame([&] { compact_signal_table(ui, s, 150); });
    contains(signals, "Observed at (UTC)"); contains(signals, "2023-11-14 22:13:20.125");
    contains(signals, "2023-11-15 22:13:20.875");
    const auto advanced = canvas.frame([&] { waveform_panel(s, 180); });
    contains(advanced, "Observed at (UTC)"); contains(advanced, "2023-11-14 22:13:20.125");
    contains(advanced, "1.071234");
    require(s.waveforms[0].delimiter_utc == 1700000000.125 && s.receptions[0].elapsed_seconds == 5.25,
        "Historical rendering leaves recorded timestamps and durations unchanged");
}

void actual_waveform_rendering(Canvas& canvas) {
    auto s = observations_fixture();
    canvas.frame([&] { waveform_panel(s, 180); });
    const auto text = canvas.frame([&] { open_details(); waveform_panel(s, 180); });
    for (const auto* expected : {"INFERRED MODEM SETTINGS", "864.123456", "867.321988", "250", "500",
            "SF11", "SF7", "1.071234", "3.103456", "Mesh protocol: unknown", "Payload: not decoded",
            "not packet airtime", "association ambiguous", "Range edge / incomplete", "not yet validated"})
        contains(text, expected);
    require(text.find("906.875") == std::string::npos && text.find("908.750") == std::string::npos &&
        text.find("LongFast") == std::string::npos && text.find("LongTurbo") == std::string::npos,
        "Waveform rendering has no preset-frequency dependence");
    require(text.find("16.00") == std::string::npos, "Energy span is not used as inferred modem bandwidth");

    s.waveforms.clear();
    contains(canvas.frame([&] { waveform_panel(s, 100); }), "No matching waveform observations yet");
    s.discovery.enabled = false; s.config.discover_lora = false; s.running = false;
    contains(canvas.frame([&] { waveform_panel(s, 100); }), "Enable Discover LoRa waveforms");
    contains(canvas.frame([&] { waveform_panel(s, 100, true); }), "configured for the next survey");
}

void coverage_and_selected_results(Canvas& canvas) {
    auto s = observations_fixture(); s.discovery.failed = true; s.discovery.finished = true;
    s.discovery.fault = "source-generated worker failure";
    s.discovery.rejected_input_samples = 1234; s.discovery.abandoned_input_samples = 77;
    s.discovery.source_queue_drops = 3; s.discovery.result_overflows = 4;
    s.discovery.gap_overflows = 5; s.discovery.stream_resets = 6;
    DiscoveryBandCoverage band; band.subband_index = 1; band.center_hz = 865000000;
    band.processed_samples = 2000000; band.abandoned_samples = 7;
    band.source_gap_input_samples = 800;
    band.candidate_limit_hits = 8; band.track_limit_hits = 9; s.discovery.bands = {band};
    const auto text = canvas.frame([&] { open_details(); waveform_panel(s, 180); });
    for (const auto* expected : {"WAVEFORM DISCOVERY FAILED", "source-generated worker failure",
            "DISCOVERY COVERAGE LOSS", "Rejected input: 1234", "abandoned input: 77", "queue drops: 3",
            "Result overflows: 4", "gap-report overflows: 5", "stream resets: 6", "8 candidate limits",
            "9 track limits", "7 abandoned subband samples", "subband gaps are recorded", "separate from spectrum occupancy coverage"})
        contains(text, expected);
    require(s.measurement_seconds == 17.25 && s.delivered_samples == 276000000 && s.spectrum_bursts == 1000,
        "Discovery status rendering does not change spectrum measurement counters");

    s.historical = true; s.running = false;
    SurveyAnalysis selected; selected.waveform_count = 10; selected.waveforms_truncated = true;
    selected.waveforms = {s.waveforms.front()};
    const auto queried = canvas.frame([&] { waveform_panel(s, 180, false, &selected); });
    contains(queried, "saved session"); contains(queried, "10 waveform observations match");
    contains(queried, "latest completed analysis query"); contains(queried, "result list is limited");
    contains(queried, "864.123456");
    require(queried.find("867.321988") == std::string::npos,
        "Selected analysis does not mix in unfiltered recent session observations");
    selected.waveforms.clear(); selected.waveform_count = 0; selected.waveforms_truncated = false;
    contains(canvas.frame([&] { waveform_panel(s, 100, false, &selected); }), "No waveform observations match this query");
}

void discovery_setting_persists_without_decoder_or_hardware(Canvas& canvas) {
    namespace fs = std::filesystem;
    const auto folder = fs::current_path() / ("waveform-ui-fixture-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct RemoveFixture { fs::path path; ~RemoveFixture() { std::error_code e; fs::remove_all(path, e); } } cleanup{folder};
    const auto utf8 = folder.u8string(); const std::string path(utf8.begin(), utf8.end());
    DesktopState initial; initial.initialize_preferences(path, false);
    require(initial.preferences_ready && initial.config.discover_lora && initial.config.lanes.size()==1 && initial.decode_enabled && !initial.spectrum_only,
        "Fresh ordinary setup enables discovery and arms one explicitly keyed decoder profile");
    initial.persist_preferences();
    DesktopState reopened; reopened.initialize_preferences(path, false);
    require(reopened.preferences_ready && reopened.config.discover_lora && reopened.config.lanes.size()==1,
        "Discovery selection and the default armed profile survive ordinary setup reload");
    Engine engine; Snapshot idle;
    const auto text = canvas.frame([&] { receiver_controls(engine, reopened, idle); });
    require(text.find("Spectrum only")==std::string::npos && text.find("Discover LoRa waveforms")==std::string::npos,
        "Discovery and decode-mode controls are removed from the operating receiver panel");
    const auto settings = canvas.frame([&] { detection_settings(engine, reopened, idle); });
    contains(settings, "Spectrum only"); contains(settings, "Discover LoRa waveforms");
    contains(settings, "Decode authorized messages");
    require(!engine.snapshot().running && engine.configured_key_count()==0,
        "Rendering setup never opens a receiver or implicitly configures a public channel key");
    reopened.config.discover_lora = false; reopened.persist_preferences();
    DesktopState opted_out; opted_out.initialize_preferences(path, false);
    require(!opted_out.config.discover_lora && opted_out.config.lanes.size()==1, "Explicit discovery opt-out persists independently of the armed profile");
    DesktopState launch_override; launch_override.config.discover_lora = true;
    launch_override.initialize_preferences(path, false);
    require(launch_override.config.discover_lora, "Explicit launch discovery request takes precedence over saved opt-out");
}

void small_window_shows_modem_rows(Canvas& canvas) {
    ImGui::GetIO().DisplaySize = {962, 769};
    Engine engine; DesktopState ui; auto s = observations_fixture(); ui.config = s.config;
    s.spectrum_dbfs.assign(4096, -90); s.spectrum_sequence = 1;
    // Render the actual Live RF workspace with normal collapsed help and no
    // logging auto-expansion. No GLFW window is created by render().
    for (int pass = 0; pass < 2; ++pass) canvas.frame([&] { ovmesh::render(engine, ui, s); }, false);
    ImGuiWindow* result_window = nullptr;
    for (auto* window : ImGui::GetCurrentContext()->Windows) {
        const std::string_view name(window->Name);
        const auto result_component = name.rfind("/liveResults_");
        // The scrolling table is itself a child window whose full name also
        // contains liveResults. Resolve the actual result pane, not that child.
        if (result_component != std::string_view::npos && name.find('/', result_component + 1) == std::string_view::npos && window->Active)
            result_window = window;
    }
    require(result_window, "Live RF result region exists at a small desktop size");
    // The selected Detected signals tab contributes an ID-stack component.
    // Resolve the active table by its owning pane rather than rebuilding that
    // internal tab ID from the outside.
    const ImGuiTable* table = nullptr;
    auto& tables = ImGui::GetCurrentContext()->Tables;
    for (int i = 0; i < tables.GetMapSize(); ++i)
        if (const auto* candidate = tables.TryGetMapData(i); candidate && candidate->OuterWindow == result_window && candidate->ColumnsCount == 5)
            table = candidate;
    if (table) std::cout << "962x769 result viewport y=" << result_window->InnerClipRect.Min.y << ".."
        << result_window->InnerClipRect.Max.y << "; full signal table y=" << table->OuterRect.Min.y << ".."
        << table->OuterRect.Max.y << '\n';
    require(table && table->OuterRect.Min.y >= result_window->InnerClipRect.Min.y &&
        table->OuterRect.Min.y + 3 * ImGui::GetTextLineHeightWithSpacing() <= result_window->InnerClipRect.Max.y,
        "BW/SF header and two observation rows are visible without scrolling at 962x769");
    require(table->OuterRect.Max.y <= result_window->InnerClipRect.Max.y,
        "The whole waveform table also fits the initial result viewport at 962x769");
}
} // namespace

int main() {
    try {
        Canvas canvas;
        reception_timestamps(canvas);
        actual_waveform_rendering(canvas);
        coverage_and_selected_results(canvas);
        discovery_setting_persists_without_decoder_or_hardware(canvas);
        small_window_shows_modem_rows(canvas);
        std::cout << "Waveform UI checks passed (metadata-only, no hardware)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Waveform UI checks failed: " << error.what() << '\n';
        return 1;
    }
}
