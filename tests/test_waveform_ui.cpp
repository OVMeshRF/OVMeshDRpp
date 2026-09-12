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
    record.decoded.status = protocol::Status::classified;
    record.decoded.classification = "likely Meshtastic";
    record.decoded.evidence = protocol::EnvelopeEvidence{1, true};
    s.receptions = {record};
    const auto log = canvas.frame([&] { packet_table(ui, s, 150); });
    contains(log, "Received at (UTC)"); contains(log, "2023-11-14 22:13:20.125");
    const auto detail = canvas.frame([&] { reception_detail(record); });
    contains(detail, "Envelope evidence: port 1");
    contains(detail, "not authenticated");
    contains(detail, "may be a false positive");
    for (const auto* removed : {"AUTHORIZED SCHEMA FIELDS", "Reported origin", "Sender-reported", "Request ID", "Content:", "Node ID", "Reported forward route"})
        require(detail.find(removed) == std::string::npos, "Reception details expose no semantic packet fields");
    contains(log, "Envelope evidence"); contains(log, "Port 1");
    copy_text(ui.filter, "likely Meshtastic");
    require(matches_filter(record, ui), "Classification remains searchable");
    contains(detail, "2023-11-14 22:13:20.125 UTC"); contains(detail, "Elapsed session time: 5.250000 s");
    const auto signals = canvas.frame([&] { compact_signal_table(ui, s, 150); });
    require(signals.empty(), "Disabled compact waveform table renders no historical observations");
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
    require(initial.preferences_ready && !initial.config.discover_lora && initial.config.lanes.empty() && !initial.decode_enabled && initial.spectrum_only,
        "Fresh ordinary setup enforces spectrum only without discovery or classification");
    initial.persist_preferences();
    DesktopState reopened; reopened.initialize_preferences(path, false);
    require(reopened.preferences_ready && !reopened.config.discover_lora && reopened.config.lanes.empty() && !reopened.decode_enabled && reopened.spectrum_only,
        "Reload preserves the spectrum-only policy without a fixed-frequency profile");
    Engine engine; Snapshot idle;
    const auto text = canvas.frame([&] { receiver_controls(engine, reopened, idle); });
    require(text.find("Spectrum only")==std::string::npos && text.find("Discover LoRa waveforms")==std::string::npos,
        "Discovery and decode-mode controls are removed from the operating receiver panel");
    const auto settings = canvas.frame([&] { detection_settings(engine, reopened, idle); });
    require(settings.empty(), "Disabled detection settings render no controls");
    require(!engine.snapshot().running && engine.configured_key_count()==0,
        "Rendering setup never opens a receiver or implicitly configures a public channel key");
    reopened.config.discover_lora = false; reopened.persist_preferences();
    DesktopState opted_out; opted_out.initialize_preferences(path, false);
    require(!opted_out.config.discover_lora && opted_out.config.lanes.empty(), "Explicit discovery opt-out persists without introducing a fixed-frequency profile");
    DesktopState launch_override; launch_override.config.discover_lora = true;
    launch_override.initialize_preferences(path, false);
    require(!launch_override.config.discover_lora && !launch_override.decode_enabled && launch_override.spectrum_only,
        "Spectrum-only policy overrides an old explicit discovery launch request");
}

void small_window_shows_energy_results(Canvas& canvas) {
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
    require(result_window->InnerClipRect.GetHeight() >= 3 * ImGui::GetTextLineHeightWithSpacing(),
        "Energy results retain a usable viewport at 962x769");
    const auto visible = canvas.frame([&] { ovmesh::render(engine, ui, s); });
    for (const auto* hidden : {"Detected signals", "Packet classifications", "LoRa receptions"})
        require(visible.find(hidden) == std::string::npos,
            "Disabled LoRa result tabs remain hidden even when a snapshot contains observations");
    contains(visible, "Energy details");

}

struct ColoredBounds {
    size_t vertices = 0;
    float top = std::numeric_limits<float>::infinity();
    float bottom = -std::numeric_limits<float>::infinity();
};

ColoredBounds rendered_color(ImU32 color) {
    ColoredBounds result;
    const auto* draw = ImGui::GetDrawData();
    for (int list = 0; list < draw->CmdListsCount; ++list)
        for (const auto& vertex : draw->CmdLists[list]->VtxBuffer)
            if (vertex.col == color) {
                ++result.vertices;
                result.top = std::min(result.top, vertex.pos.y);
                result.bottom = std::max(result.bottom, vertex.pos.y);
            }
    return result;
}

void waterfall_resize_preserves_signal_geometry(Canvas& canvas) {
    ImGui::GetIO().DisplaySize = {1600, 1100};
    for (const float scale : {1.f, 1.5f}) {
        DesktopState ui; auto snapshot = observations_fixture();
        snapshot.config.lanes.clear(); snapshot.spectrum_sequence = 20;
        snapshot.spectrum_dbfs = {-99.f, -99.f};
        ui.config = snapshot.config; ui.ui_scale = scale; ui.freeze_waterfall = true;
        ui.last_session = snapshot.session_id; ui.last_spectrum = snapshot.spectrum_sequence;
        ui.waterfall_center_hz = snapshot.config.center_hz;
        ui.waterfall_span_hz = snapshot.config.survey_span_hz;
        ui.spectrum_height = 80; ui.waterfall_height = 100;
        ui.waterfall.assign(140, std::vector<float>{-99.f, -99.f});
        ui.waterfall[10] = {-35.f, -35.f};
        ui.waterfall[80] = {-50.f, -50.f};
        const auto retained = ui.waterfall;
        const auto event_color = heat_color(-35.f, ui.display_floor, ui.display_ceiling);
        const auto older_color = heat_color(-50.f, ui.display_floor, ui.display_ceiling);
        const auto render = [&] { canvas.frame([&] { spectrum_view(ui, snapshot, 900); }, false); };
        render();
        const auto initial = rendered_color(event_color);
        require(initial.vertices == 8 && rendered_color(older_color).vertices == 0,
            "Short waterfall renders the recent event and clips the older event");
        const float waterfall_top = ui.spectrum_grabber.y + 12 * scale;
        require(std::abs(initial.top - waterfall_top - 20 * scale) < .01f &&
            std::abs(initial.bottom - initial.top - (2 * scale + .5f)) < .01f,
            "Actual event rectangles use two logical pixels per row with a half-pixel seam overlap");
        const auto initial_capture = waveform_framebuffer_crop(ui.capture_origin, ui.capture_size,
            *ImGui::GetDrawData(), 3200, 2200);
        ui.waterfall_height = 220;
        render();
        const auto taller = rendered_color(event_color);
        require(taller.vertices == initial.vertices && taller.top == initial.top && taller.bottom == initial.bottom,
            "Making the waterfall taller does not move or stretch the same signal");
        require(rendered_color(older_color).vertices == 8,
            "A taller waterfall reveals retained older rows");
        const auto taller_capture = waveform_framebuffer_crop(ui.capture_origin, ui.capture_size,
            *ImGui::GetDrawData(), 3200, 2200);
        require(taller_capture.width == initial_capture.width &&
            taller_capture.height - initial_capture.height == static_cast<int>(240 * scale),
            "Retina PNG crop follows the visible panel height, not the retained history length");
        ui.waterfall_height = 80;
        render();
        const auto shorter = rendered_color(event_color);
        require(shorter.top == initial.top && shorter.bottom == initial.bottom &&
            rendered_color(older_color).vertices == 0,
            "Making the waterfall shorter clips history without squashing recent signals");
        ui.spectrum_height += 30;
        render();
        const auto translated = rendered_color(event_color);
        require(std::abs(translated.top - initial.top - 30 * scale) < .01f &&
            translated.bottom - translated.top == initial.bottom - initial.top,
            "Resizing the spectrum translates the waterfall without changing its row thickness");
        ui.waterfall_height = 320;
        render();
        const auto background = rendered_color(heat_color(-99.f, ui.display_floor, ui.display_ceiling));
        const float viewport_bottom = ui.waterfall_grabber.y - 22 * scale;
        require(background.bottom < viewport_bottom - 30 * scale,
            "A viewport taller than retained history leaves empty space instead of stretching old rows");
        require(ui.waterfall == retained && ui.last_spectrum == snapshot.spectrum_sequence,
            "Every resize preserves the complete frozen history buffer and update cursor");

        snapshot.spectrum_sequence += 7; snapshot.spectrum_dbfs = {-42.f, -42.f};
        render();
        require(ui.waterfall == retained, "Frozen display does not append new publications during resizing");
        ui.freeze_waterfall = false;
        render();
        require(ui.waterfall.size() == retained.size() + 1 && ui.waterfall.front() == snapshot.spectrum_dbfs &&
            ui.waterfall[1] == retained.front(),
            "Unfreezing appends only the latest actual update, without fabricating missed history");
        const auto resumed = ui.waterfall;
        render(); snapshot.running = false; render();
        require(ui.waterfall == resumed, "Repeated renders and stopped reception do not duplicate a display update");

        ui.waterfall.assign(waterfall_rows, std::vector<float>{-90.f});
        ui.waterfall.back() = {-91.f};
        ++snapshot.spectrum_sequence;
        render();
        require(ui.waterfall.size() == waterfall_rows && ui.waterfall.front() == snapshot.spectrum_dbfs &&
            ui.waterfall.back() == std::vector<float>{-90.f},
            "Only a new publication evicts the oldest row at the bounded history limit");
        ++snapshot.config.center_hz;
        render();
        require(ui.waterfall.size() == 1 && ui.waterfall.front() == snapshot.spectrum_dbfs,
            "A changed frequency axis clears incompatible waterfall history");
        ++snapshot.spectrum_sequence; render();
        require(ui.waterfall.size() == 2, "New updates accumulate on the new frequency axis");
        ++snapshot.config.survey_span_hz; render();
        require(ui.waterfall.size() == 1, "A changed frequency span clears incompatible history");
        ++snapshot.spectrum_sequence; render();
        snapshot.session_id = "source-generated-new-waterfall-session";
        render();
        require(ui.waterfall.size() == 1, "An explicit new session clears prior-session display history");
    }
}
} // namespace

int main() {
    try {
        Canvas canvas;
        reception_timestamps(canvas);
        actual_waveform_rendering(canvas);
        coverage_and_selected_results(canvas);
        discovery_setting_persists_without_decoder_or_hardware(canvas);
        small_window_shows_energy_results(canvas);
        waterfall_resize_preserves_signal_geometry(canvas);
        std::cout << "Waveform UI checks passed (metadata-only, no hardware)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Waveform UI checks failed: " << error.what() << '\n';
        return 1;
    }
}
