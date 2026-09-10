// SPDX-License-Identifier: GPL-3.0-or-later
// Approved desktop workflow using synthetic input and in-memory ImGui only.
// No GLFW window, device discovery, USB, actual keys, or ordinary preferences.
#include "../src/ui.cpp"
#include <imgui_internal.h>
#include <fstream>
#include <iostream>

namespace {
using namespace ovmesh;
namespace fs = std::filesystem;
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
void contains(const std::string& text, const char* expected) {
    require(text.find(expected) != std::string::npos, std::string("Missing UI text: ") + expected + "\n" + text);
}
struct Fixture {
    fs::path folder = fs::current_path() / ("ui-workflow-fixture-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { require(fs::create_directory(folder), "Create isolated workflow fixture"); }
    ~Fixture() { std::error_code error; fs::remove_all(folder, error); }
    std::string path(const char* name) const { return path_utf8(folder / name); }
};
std::string contents(const std::string& path) {
    std::ifstream file(fs::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    require(file.good(), "Read synthetic recording for preservation comparison");
    return {std::istreambuf_iterator<char>(file), {}};
}
class Canvas {
public:
    Canvas() {
        ImGui::CreateContext(); auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.DisplaySize = {1440, 960}; io.DeltaTime = 1.f / 60;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.AddFocusEvent(true);
        unsigned char* pixels; int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        require(pixels && width > 0 && height > 0, "Create memory-only font atlas");
    }
    ~Canvas() { ImGui::DestroyContext(); }
    template<class Render> std::string frame(Render draw) {
        ImGui::NewFrame(); ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Workflow fixture", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
        ImGui::LogToBuffer(); draw();
        const std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish(); ImGui::End(); ImGui::Render();
        validate();
        return text;
    }
    template<class Render> void full_frame(Render draw) {
        ImGui::NewFrame(); draw(); ImGui::Render(); validate();
    }
private:
    void validate() {
        const auto* context = ImGui::GetCurrentContext();
        require(context->CurrentWindowStack.empty() && context->BeginPopupStack.empty() && !context->CurrentTable,
            "Workflow balances ImGui scopes");
        const auto* data = ImGui::GetDrawData();
        require(data && data->Valid && data->TotalVtxCount > 0, "Workflow produces a rendered frame");
        for (const auto* list : data->CmdLists) for (const auto& vertex : list->VtxBuffer)
            require(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y), "Workflow geometry remains finite");
    }
};
void settle_operation(DesktopState& ui) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (ui.operation_busy() && std::chrono::steady_clock::now() < deadline) {
        ui.finish_operation(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(!ui.operation_busy(), "Desktop asynchronous operation completes within the fixture budget");
}
void measurements(Engine& engine) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = engine.snapshot(); require(snapshot.error.empty(), snapshot.error);
        if (snapshot.measurement_seconds >= .05) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("Synthetic workflow did not produce measurements");
}
void prepare_synthetic(DesktopState& ui) {
    ui.source = 0; ui.config.sample_rate = 8000000; ui.config.survey_span_hz = 5000000;
    ui.config.discover_lora = false; ui.spectrum_only = true;
}

void fresh_layout_and_settings(Canvas& canvas, const Fixture& fixture) {
    Engine engine; DesktopState ui;
    require(ui.config.lanes.empty() && !ui.preferences_active, "Passive state stays isolated from ordinary defaults");
    ui.initialize_preferences(fixture.path("fresh-profile"), false);
    require(ui.preferences_ready && ui.gps_enabled && ui.save_session && ui.config.compact_recording &&
        ui.config.discover_lora && ui.decode_enabled && !ui.spectrum_only && ui.config.lanes.size() == 1,
        "Ordinary setup applies the approved capture and decoding defaults");
    require(!engine.snapshot().running && engine.snapshot().session_id.empty() && engine.configured_key_count() == 0 &&
        engine.gps_connection_status().state == GpsConnectionState::Disconnected && ui.gps_devices.devices.empty(),
        "Metadata-only initialization starts neither reception nor GPS and adds no key");
    const auto show = [&] { render(engine, ui, engine.snapshot()); };
    canvas.frame(show); const auto main = canvas.frame(show);
    for (const char* text : {"New session", "Open...", "Save session", "Settings", "Offset / Hz", "RF amplifier", "Capture PNG..."}) contains(main, text);
    for (const char* setting : {"Discover LoRa waveforms", "Advanced / Legacy decode profiles", "Start GPS automatically",
            "Browse recording location", "FIXED OR MOBILE RECEIVER POSITION", "INPUT AVAILABILITY"})
        require(main.find(setting) == std::string::npos, "Setup and verbose diagnostics stay out of the default working screen");
    const auto detection = canvas.frame([&] { detection_settings(engine, ui, engine.snapshot()); });
    for (const char* text : {"Spectrum + LoRa", "Spectrum only", "Discover LoRa waveforms", "Decode authorized messages",
            "Advanced / Legacy decode profiles", "Automatic decoding of discovered signals is still in development"}) contains(detection, text);
    ui.show_settings = true; ui.settings_page = 2;
    canvas.frame(show); const auto recording = canvas.frame(show);
    contains(recording, "Recording mode"); contains(recording, "Browse recording location");
    ui.settings_page = 1;
    canvas.frame(show); const auto gps = canvas.frame(show);
    contains(gps, "Start GPS automatically"); contains(gps, "FIXED OR MOBILE RECEIVER POSITION");
    require(engine.gps_connection_status().state == GpsConnectionState::Disconnected && !engine.snapshot().running,
        "Opening Settings alone performs no device connection");
    ui.show_settings = false;
}

void persisted_setup_and_effective_modes(const Fixture& fixture) {
    Engine engine; DesktopState ui; ui.initialize_preferences(fixture.path("mode-profile"), false);
    prepare_synthetic(ui); ui.config.lanes.front().label = "Operator profile";
    ui.config.discover_lora = true; ui.decode_enabled = true;
    ui.start(engine, false); require(!ui.notice_error, ui.notice); measurements(engine);
    auto live = engine.snapshot();
    require(!live.config.discover_lora && live.config.lanes.empty() && ui.config.discover_lora &&
        ui.config.lanes.size() == 1 && ui.config.lanes.front().label == "Operator profile" && ui.decode_enabled,
        "Spectrum only suppresses effective discovery and decoding without erasing configured choices");
    engine.stop();
    ui.spectrum_only = false; ui.decode_enabled = false; ui.config.discover_lora = false;
    ui.start(engine, false); require(!ui.notice_error, ui.notice); measurements(engine);
    require(engine.snapshot().config.lanes.empty() && ui.config.lanes.size() == 1,
        "Disabling payload decoding preserves the editable receive profile");
    engine.stop();
    ui.decode_enabled = true;
    ui.start(engine, false); require(!ui.notice_error, ui.notice); measurements(engine);
    require(engine.snapshot().config.lanes.size() == 1 && engine.snapshot().config.lanes.front().label == "Operator profile",
        "Re-enabling authorized decoding restores the configured profile to the effective session");
    engine.stop();
    ui.config.center_hz = 868750000; ui.config.tuning_offset_hz = 900;
    ui.start(engine, false); require(!ui.notice_error, ui.notice); measurements(engine);
    require(engine.snapshot().config.lanes.empty() && ui.config.lanes.size() == 1 &&
        ui.config.lanes.front().frequency_hz == 906875000,
        "A legacy profile outside the supplied range does not block surveying or erase its saved setup");
    engine.stop();
    ui.config.lna_gain = 32; ui.config.vga_gain = 24; ui.config.amplifier = true;
    ui.mixed_fonts = false; ui.position_view_mode = PositionViewMode::Mobile;
    ui.decode_enabled = false; ui.spectrum_only = true; ui.gps_enabled = false; ui.persist_preferences();
    require(ui.preferences_error.empty(), ui.preferences_error);
    DesktopState reopened; reopened.initialize_preferences(fixture.path("mode-profile"), false);
    require(reopened.config.center_hz == ui.config.center_hz && reopened.config.tuning_offset_hz == 900 &&
        reopened.config.sample_rate == ui.config.sample_rate && reopened.config.survey_span_hz == ui.config.survey_span_hz &&
        reopened.config.lna_gain == 32 && reopened.config.vga_gain == 24 && reopened.config.amplifier && reopened.source == 0 &&
        !reopened.decode_enabled && reopened.spectrum_only && !reopened.config.discover_lora && !reopened.gps_enabled &&
        !reopened.mixed_fonts && reopened.position_view_mode == PositionViewMode::Mobile,
        "Fresh launch retains receiver setup, explicit opt-outs and display choices");
    require(reopened.waterfall.empty() && !reopened.analysis_loaded && reopened.config.session_path.empty() &&
        !fs::exists(fs::path(reopened.session_path)), "Fresh launch never restores prior results or opens their recording");
}

void save_new_and_historical(Canvas& canvas, const Fixture& fixture) {
    Engine engine; DesktopState ui; ui.initialize_preferences(fixture.path("session-profile"), false);
    prepare_synthetic(ui); ui.start(engine, false); require(!ui.notice_error, ui.notice); measurements(engine);
    const auto recorded_path = engine.snapshot().config.session_path;
    request_save_session(engine, ui); require(ui.operation_busy(), "Save is dispatched without blocking the render thread");
    settle_operation(ui);
    require(!ui.notice_error && engine.snapshot().running && fs::exists(fs::path(recorded_path)),
        "Asynchronous Save confirms a recording checkpoint and leaves reception running");
    engine.stop(); const auto stopped = engine.snapshot();
    require(stopped.spectrum_tiles > 0 && !stopped.running && !stopped.frequencies.empty(), "Stop retains measured results");
    const auto original = contents(recorded_path);
    ui.waterfall.push_back({-80,-65}); ui.analysis_loaded = true; ui.analysis_session = stopped.session_id;
    ui.freeze_waterfall = true;
    request_new_session(engine, ui); settle_operation(ui);
    require(!ui.notice_error && engine.snapshot().session_id.empty() && engine.snapshot().frequencies.empty() &&
        ui.waterfall.empty() && !ui.analysis_loaded && !ui.freeze_waterfall && !engine.snapshot().running &&
        ui.config.session_path.empty() && ui.session_path != recorded_path,
        "New finalizes the prior recording and clears all displayed results without starting RF");
    require(contents(recorded_path) == original, "New leaves the previous recording byte-identical");

    ui.config.center_hz = 433750000; ui.config.tuning_offset_hz = -700;
    const auto next_config = ui.config;
    Snapshot source = stopped;
    open_session_chooser(engine, ui, source);
    require(ui.file_picker.purpose == FilePickerPurpose::OpenSurvey && ui.file_chosen,
        "Toolbar Open installs the actual saved-session selection handler");
    // Supply the fixture path just as the file-picker completion does.
    auto chosen = std::move(ui.file_chosen); ui.file_chosen = {}; ui.file_picker.purpose = FilePickerPurpose::None;
    chosen(recorded_path); settle_operation(ui);
    require(!ui.notice_error && engine.snapshot().historical && ui.focus_analysis &&
        ui.config.center_hz == next_config.center_hz && ui.config.tuning_offset_hz == next_config.tuning_offset_hz,
        "Historical Open is read-only and keeps next-session receiver configuration separate");
    canvas.frame([&] { receiver_controls(engine, ui, engine.snapshot()); });
    require(ui.config.center_hz == next_config.center_hz && ui.config.tuning_offset_hz == next_config.tuning_offset_hz &&
        contents(recorded_path) == original, "Rendering recorded receiver settings changes neither next-session setup nor historical file");
    request_new_session(engine, ui); settle_operation(ui);
    require(!ui.notice_error && !engine.snapshot().historical && engine.snapshot().session_id.empty() &&
        ui.config.center_hz == next_config.center_hz, "New returns from historical viewing to retained operator setup");
}

void unrecorded_and_pending_feedback(Canvas& canvas) {
    Engine engine; DesktopState ui; prepare_synthetic(ui); ui.save_session = false;
    ui.start(engine, false); require(!ui.notice_error, ui.notice); measurements(engine); engine.stop();
    const auto previous = engine.snapshot();
    request_save_session(engine, ui); settle_operation(ui);
    require(ui.notice_error && engine.snapshot().session_id == previous.session_id, "Save cannot invent history for memory-only data");
    request_new_session(engine, ui); settle_operation(ui);
    require(ui.notice_error && engine.snapshot().session_id == previous.session_id && !engine.snapshot().frequencies.empty(),
        "New without discard consent preserves stopped memory-only measurements");
    request_new_session(engine, ui, true); settle_operation(ui);
    require(!ui.notice_error && engine.snapshot().session_id.empty(), "Explicit discard confirmation clears memory-only results");

    std::promise<void> release; auto barrier = release.get_future().share();
    bool completed = false;
    ui.begin_operation("Saving session checkpoint...", [barrier] {
        barrier.wait(); return DesktopState::OperationResult{true, "Checkpoint fixture completed"};
    }, [&] { completed = true; });
    bool ignored_work = false;
    ui.begin_operation("Unexpected overlapping operation", [&] { ignored_work = true; return DesktopState::OperationResult{}; });
    const auto main = canvas.frame([&] { render(engine, ui, engine.snapshot()); });
    contains(main, "Saving session checkpoint...");
    require(ui.operation_busy() && !completed && !ignored_work && ui.operation_label == "Saving session checkpoint...",
        "Pending save stays visibly busy while frames render and overlapping operations are rejected");
    ui.finish_operation(); require(!completed, "Pending operation is polled without waiting on its worker");
    release.set_value(); settle_operation(ui);
    require(completed && !ui.notice_error && ui.operation_label.empty(), "Completion callback and feedback run after the asynchronous result");
}

void asynchronous_analysis_preserves_selection(Canvas& canvas, const Fixture& fixture) {
    Engine engine; DesktopState ui; ui.initialize_preferences(fixture.path("analysis-profile"), false);
    prepare_synthetic(ui); ui.start(engine, false); require(!ui.notice_error, ui.notice); measurements(engine); engine.stop();
    const auto recorded = engine.snapshot();
    const auto bytes = contents(recorded.config.session_path);
    const auto settle = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (ui.analysis_busy() && std::chrono::steady_clock::now() < deadline) {
            progress_analysis(engine, ui);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(!ui.analysis_busy(), "Actual asynchronous analysis completes within the bounded fixture budget");
    };
    ui.query_lower_mhz = 906.75; ui.query_upper_mhz = 907;
    queue_analysis(ui, recorded, false); progress_analysis(engine, ui);
    require(ui.analysis_operation.valid() && !ui.pending_analysis && !ui.analysis_loaded,
        "Native progress helper dispatches the query as a future instead of applying it synchronously");
    settle();
    require(ui.analysis_loaded && ui.analysis_session == recorded.session_id && ui.analysis.observed_seconds > 0 &&
        ui.analyzed_query.lower_hz == 906750000 && ui.analyzed_query.upper_hz == 907000000,
        "Completed query applies the correct recorded session and selected frequency interval");
    const auto previous_caption = analysis_frequency_text(ui.analysis);
    const double previous_lower = ui.analysis.covered_lower_hz;
    ui.query_lower_mhz = 908.5; ui.query_upper_mhz = 909;
    queue_analysis(ui, recorded, true); progress_analysis(engine, ui);
    require(ui.analysis_operation.valid() && ui.analysis_loaded && ui.analysis.covered_lower_hz == previous_lower &&
        ui.analyzed_query.lower_hz == 906750000, "Pending query preserves the last completed plotted selection");
    const auto pending = canvas.frame([&] { compact_analysis_tab(engine, ui, recorded); });
    contains(pending, previous_caption.c_str());
    require(ui.analysis_operation.valid() && ui.analysis.covered_lower_hz == previous_lower,
        "The analysis workspace renders while a query result is pending without replacing its previous measurements");
    settle();
    require(ui.analysis_loaded && ui.analysis_session == recorded.session_id && ui.reveal_time_plot &&
        ui.analyzed_query.lower_hz == 908500000 && ui.analysis.covered_lower_hz > previous_lower,
        "The next completed query updates the selected interval and its linked-view request");

    ui.query_lower_mhz = 906.75; ui.query_upper_mhz = 907;
    queue_analysis(ui, recorded, false); progress_analysis(engine, ui);
    require(ui.analysis_operation.valid(), "Prepare an actual old-session query result");
    require(ui.analysis_operation.wait_for(std::chrono::seconds(6)) == std::future_status::ready,
        "Old-session query finishes before the controlled New transition");
    // Leave the completed future unconsumed, matching a result that arrives
    // while New is being handled on the event loop.
    request_new_session(engine, ui); settle_operation(ui);
    require(!ui.notice_error && engine.snapshot().session_id.empty() && !ui.analysis_loaded,
        "New clears the view while retaining the completed old-session future for safe disposal");
    progress_analysis(engine, ui);
    require(!ui.analysis_busy() && !ui.analysis_loaded && ui.analysis_session.empty() && ui.analysis.bins.empty(),
        "An old-session result cannot overwrite the fresh view after New");
    queue_analysis(ui, recorded, false); progress_analysis(engine, ui);
    require(!ui.analysis_busy() && !ui.analysis_loaded,
        "A queued request for a different session is rejected before starting another query");
    require(contents(recorded.config.session_path) == bytes,
        "Asynchronous queries and New leave the original recording byte-identical");
}

void scaled_settings_scopes(Canvas& canvas) {
    Engine engine; DesktopState ui; ui.passive_smoke = true;
    ImGui::GetIO().DisplaySize = {1000, 680}; ui.ui_scale = 1.5f; ui.show_settings = true;
    for (int page = 0; page < 6; ++page) {
        ui.settings_page = page;
        canvas.frame([&] { render(engine, ui, engine.snapshot()); });
        const auto text = canvas.frame([&] { render(engine, ui, engine.snapshot()); });
        contains(text, "Settings"); contains(text, "Done");
    }
    require(!engine.snapshot().running && engine.gps_connection_status().state == GpsConnectionState::Disconnected,
        "All Settings categories render at the minimum window size and 1.5 scale without opening devices");
    ui.show_settings = false; ui.ui_scale = 1;
    ImGui::GetIO().DisplaySize = {1440, 960};
    canvas.frame([&] { render(engine, ui, engine.snapshot()); });
}

void toolbar_open_displays_chooser(Canvas& canvas, const Fixture& fixture) {
    Engine engine; DesktopState ui; prepare_synthetic(ui); ui.save_session = false;
    ui.reopen_path = fixture.path("selection.sqlite");
    const auto draw = [&] { canvas.full_frame([&] { render(engine, ui, engine.snapshot()); }); };
    const auto key = [&](ImGuiKey key) {
        ImGui::GetIO().AddKeyEvent(key, true); draw();
        ImGui::GetIO().AddKeyEvent(key, false); draw();
    };
    const auto navigate = [&](ImGuiWindow* window, ImGuiID target) {
        ImGui::FocusWindow(window);
        for (unsigned attempt = 0; attempt < 64 && ImGui::GetCurrentContext()->NavId != target; ++attempt)
            key(ImGuiKey_Tab);
        require(ImGui::GetCurrentContext()->NavId == target, "Navigation reaches the actual workflow button");
    };
    for (const bool stopped : {false, true}) {
        if (stopped) {
            ui.start(engine, false); require(!ui.notice_error, ui.notice); measurements(engine); engine.stop();
        }
        const auto before = engine.snapshot(); draw(); draw();
        auto* main = ImGui::FindWindowByName("OVMeshDR++");
        require(main && main->Active, "Full desktop window is active");
        // Table widgets have a table ID scope. Navigate to obtain the actual
        // rectangle, then exercise real mouse down/up rather than calling Open.
        const auto target = ImHashStr("Open...", 0, main->GetID("sessionToolbar"));
        navigate(main, target);
        const auto rect = main->NavRectRel[ImGuiNavLayer_Main];
        const ImVec2 point{main->Pos.x + (rect.Min.x + rect.Max.x) / 2,
                           main->Pos.y + (rect.Min.y + rect.Max.y) / 2};
        ImGui::GetIO().AddMousePosEvent(point.x, point.y); draw();
        ImGui::GetIO().AddMouseButtonEvent(0, true); draw();
        ImGui::GetIO().AddMouseButtonEvent(0, false); draw(); draw();
        auto* chooser = ImGui::FindWindowByName("Choose a local file");
        require(chooser && chooser->Active && !chooser->Hidden &&
            ui.file_picker.purpose == FilePickerPurpose::OpenSurvey && !ui.file_picker.request_open && ui.file_chosen,
            "Mouse-clicking toolbar Open shows the file chooser in a fresh or stopped full desktop frame");
        require(!ImGui::GetCurrentContext()->OpenPopupStack.empty() && !ui.operation_busy(),
            "Open displays a modal before any session read or replacement begins");
        navigate(chooser, chooser->GetID("Cancel")); key(ImGuiKey_Enter); draw();
        require(ui.file_picker.purpose == FilePickerPurpose::None && !ui.file_chosen &&
            engine.snapshot().session_id == before.session_id && !engine.snapshot().running,
            "Cancelling the actual chooser preserves the fresh or stopped session");
        ImGui::GetIO().AddMousePosEvent(-1000, -1000); draw();
    }
}

void final_analysis_refresh_after_stop(Canvas& canvas, const Fixture& fixture) {
    Engine engine; DesktopState ui; ui.initialize_preferences(fixture.path("final-analysis-profile"), false);
    prepare_synthetic(ui); ui.start(engine, false); require(!ui.notice_error, ui.notice); measurements(engine);
    request_save_session(engine, ui); settle_operation(ui); require(!ui.notice_error, ui.notice);
    const auto settle_analysis = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (ui.analysis_busy() && std::chrono::steady_clock::now() < deadline) {
            progress_analysis(engine, ui); std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(!ui.analysis_busy() && ui.analysis_loaded, "Final-refresh fixture analysis completes");
    };
    canvas.frame([&] { compact_analysis_tab(engine, ui, engine.snapshot()); });
    require(ui.pending_analysis && ui.last_analysis_requested_while_running,
        "Running analysis records that its last query predates Stop");
    settle_analysis(); engine.stop(); const auto stopped = engine.snapshot();
    const auto before = ui.last_analysis_refresh;
    canvas.frame([&] { compact_analysis_tab(engine, ui, stopped); });
    require(ui.pending_analysis && !ui.last_analysis_requested_while_running &&
        ui.last_analysis_refresh > before && ui.last_analysis_refresh - before < 5,
        "Stop queues one final refresh immediately, even inside the live five-second refresh interval");
    settle_analysis(); const auto final_refresh = ui.last_analysis_refresh;
    const auto final_seconds = ui.analysis.observed_seconds;
    for (unsigned frame = 0; frame < 3; ++frame)
        canvas.frame([&] { compact_analysis_tab(engine, ui, stopped); });
    require(!ui.analysis_busy() && ui.last_analysis_refresh == final_refresh &&
        ui.analysis.observed_seconds == final_seconds && ui.analysis_session == stopped.session_id,
        "Stopped analysis remains stable after the single final refresh completes");
}
}
int main() {
    try {
        Fixture fixture; Canvas canvas;
        fresh_layout_and_settings(canvas, fixture);
        persisted_setup_and_effective_modes(fixture);
        save_new_and_historical(canvas, fixture);
        unrecorded_and_pending_feedback(canvas);
        asynchronous_analysis_preserves_selection(canvas, fixture);
        scaled_settings_scopes(canvas);
        toolbar_open_displays_chooser(canvas, fixture);
        final_analysis_refresh_after_stop(canvas, fixture);
        std::cout << "Desktop workflow checks passed; synthetic input only, no windows or devices\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "Desktop workflow checks failed: " << error.what() << '\n'; return 1; }
}
