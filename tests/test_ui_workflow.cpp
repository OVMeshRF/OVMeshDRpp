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
        !ui.config.discover_lora && !ui.config.automatic_decode && !ui.decode_enabled && ui.spectrum_only && ui.config.lanes.empty(),
        "Ordinary setup arms spectrum recording and GPS with all LoRa processing disabled");
    require(!engine.snapshot().running && engine.snapshot().session_id.empty() && engine.configured_key_count() == 0 &&
        engine.gps_connection_status().state == GpsConnectionState::Disconnected && ui.gps_devices.devices.empty(),
        "Metadata-only initialization starts neither reception nor GPS and adds no key");
    ui.apply_public_key(engine);
    require(!engine.public_meshtastic_key_enabled() && engine.configured_key_count() == 0,
        "Spectrum-only desktop adds no public classification key");
    const auto show = [&] { render(engine, ui, engine.snapshot()); };
    canvas.frame(show); const auto main = canvas.frame(show);
    for (const char* text : {"New", "Open...", "Save", "Settings", "Offset / Hz", "RF amplifier", "Capture PNG...", "Energy details"}) contains(main, text);
    for (const char* setting : {"Discover LoRa waveforms", "Advanced / Legacy decode profiles", "Start GPS automatically",
            "Browse recording location", "FIXED OR MOBILE RECEIVER POSITION", "INPUT AVAILABILITY", "Detected signals",
            "Packet classifications", "Classification settings", "LoRa receptions"})
        require(main.find(setting) == std::string::npos, "Setup and verbose diagnostics stay out of the default working screen");
    const auto detection = canvas.frame([&] { detection_settings(engine, ui, engine.snapshot()); });
    require(detection.empty(), "Disabled detection settings cannot reveal controls even when called directly");
    ui.show_settings = true; ui.settings_page = 2;
    canvas.frame(show); const auto recording = canvas.frame(show);
    contains(recording, "Recording mode"); contains(recording, "Browse recording location");
    ui.settings_page = 0; // A stale UI selection must not reopen hidden controls.
    canvas.frame(show); const auto gps = canvas.frame(show);
    require(ui.settings_page == 1 && gps.find("Detection & classification") == std::string::npos,
        "Settings preserves category IDs and redirects the hidden detection page to GPS");
    contains(gps, "Start GPS automatically"); contains(gps, "FIXED OR MOBILE RECEIVER POSITION");
    contains(gps, "GPS is optional. Reception continues if it is unavailable");
    require(engine.gps_connection_status().state == GpsConnectionState::Disconnected && !engine.snapshot().running,
        "Opening Settings alone performs no device connection");
    ui.show_settings = false;
}

void preset_catalog_and_automatic_status(Canvas& canvas) {
    const auto catalog = canvas.frame([&] { meshtastic_preset_catalog(); });
    for (const char* text : {"One public default key (AQ==)", "Preset", "BW / kHz", "SDR support", "Deprecated presets remain listed"})
        contains(catalog, text);
    const auto rak_catalog = canvas.frame([&] { meshtastic_preset_catalog(true); });
    contains(rak_catalog, "RAK support"); contains(rak_catalog, "Unsupported");
    Snapshot snapshot;
    contains(canvas.frame([&] { automatic_decoder_status(snapshot, true); }), "ready for reception");
    snapshot.running = true; snapshot.automatic_decoder.available = true; snapshot.automatic_decoder.enabled = true;
    snapshot.automatic_decoder.completed = 4; snapshot.automatic_decoder.crc_valid = 3;
    snapshot.automatic_decoder.classified = 2; snapshot.automatic_decoder.active_decoders = 2;
    snapshot.automatic_decoder.history_misses = 5;
    const auto live = canvas.frame([&] { automatic_decoder_status(snapshot, true); });
    contains(live, "running / 2 active"); contains(live, "Latest acquisition: 4 frames / 3 CRC valid / 2 likely Meshtastic");
    contains(live, "Some candidates could not be processed");
    snapshot.running = false; snapshot.historical = true; snapshot.automatic_decoder.available = false;
    const auto historical = canvas.frame([&] { automatic_decoder_status(snapshot, false); });
    contains(historical, "diagnostics were not recorded");
    require(historical.find("Latest acquisition:") == std::string::npos, "Missing historical automatic diagnostics are unavailable, not fabricated zero counters");
    snapshot.automatic_decoder.available = true;
    contains(canvas.frame([&] { automatic_decoder_status(snapshot, false); }), "recorded / 2 active");

    LaneConfig lane; lane.frequency_hz = 869123456; lane.enabled = false;
    const auto draw = [&] { return canvas.frame([&] { meshtastic_preset_picker(lane); }); };
    const auto key = [&](ImGuiKey value) {
        ImGui::GetIO().AddKeyEvent(value, true); draw();
        ImGui::GetIO().AddKeyEvent(value, false); draw();
    };
    const auto navigate = [&](ImGuiWindow* window, ImGuiID id) {
        ImGui::FocusWindow(window);
        for (unsigned attempt = 0; attempt < 64 && ImGui::GetCurrentContext()->NavId != id; ++attempt) key(ImGuiKey_Tab);
        require(ImGui::GetCurrentContext()->NavId == id, "Keyboard navigation reaches the actual preset selection");
    };
    const auto click_selected = [&](ImGuiWindow* window) {
        // ImGui stores nav rectangles relative to CursorStartPos, which includes
        // title/padding/scroll offsets. Use its conversion, not window->Pos.
        const auto rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
        ImGui::GetIO().AddMousePosEvent((rect.Min.x + rect.Max.x) * .5f,
            (rect.Min.y + rect.Max.y) * .5f); draw();
        ImGui::GetIO().AddMouseButtonEvent(0, true); draw();
        ImGui::GetIO().AddMouseButtonEvent(0, false); draw(); draw();
    };
    draw(); draw(); auto* page = ImGui::FindWindowByName("Workflow fixture");
    navigate(page, page->GetID("Meshtastic preset")); click_selected(page);
    auto* context = ImGui::GetCurrentContext();
    require(!context->OpenPopupStack.empty(), "Mouse opens the shared Meshtastic preset picker");
    auto* popup = context->OpenPopupStack.back().Window;
    navigate(popup, popup->GetID("MediumFast")); click_selected(popup);
    require(lane.label == "MediumFast" && lane.bandwidth_hz == 250000 && lane.spreading_factor == 9 && lane.coding_rate == 5 &&
        lane.frequency_hz == 869123456 && !lane.enabled && context->OpenPopupStack.empty(),
        "Preset selection applies pinned modem values while preserving operator frequency and enabled state");
    navigate(page, page->GetID("Meshtastic preset")); click_selected(page);
    require(!context->OpenPopupStack.empty(), "Preset picker reopens for a narrow 2.8 mode");
    popup = context->OpenPopupStack.back().Window;
    navigate(popup, popup->GetID("TinyFast")); click_selected(popup);
    require(lane.label == "TinyFast" && lane.bandwidth_hz == 15625 && lane.spreading_factor == 7 && lane.coding_rate == 5 &&
        lane.frequency_hz == 869123456 && !lane.enabled,
        "TinyFast selects the actual 15.625 kHz modem bandwidth without changing frequency");
}

void persisted_setup_and_effective_modes(const Fixture& fixture) {
    const auto paths = preference_paths(fixture.path("mode-profile"));
    ensure_preferences_directories(paths);
    auto previous = load_preferences(paths);
    previous.discover_lora = previous.decode_enabled = previous.public_meshtastic_key_enabled = true;
    previous.spectrum_only = false;
    previous.concentrators.decode_enabled = true;
    previous.concentrators.boards[0].packets_enabled = true;
    save_preferences(paths, previous);
    Engine engine; DesktopState ui; ui.initialize_preferences(fixture.path("mode-profile"), false);
    auto expected = previous;
    expected.spectrum_only = true;
    expected.discover_lora = expected.decode_enabled = expected.public_meshtastic_key_enabled = false;
    expected.concentrators.decode_enabled = false;
    for (auto& board : expected.concentrators.boards) board.packets_enabled = false;
    require(ui.spectrum_only && !ui.decode_enabled && !ui.public_meshtastic_key_enabled && !ui.config.discover_lora &&
        !ui.config.automatic_decode && !ui.config.concentrators.decode_enabled && !ui.config.concentrators.boards[0].packets_enabled &&
        load_preferences(paths) == expected,
        "Old enabled preferences migrate only detection switches while preserving RF, GPS, recording and display setup");
    prepare_synthetic(ui); ui.config.lanes.push_back(LaneConfig{}); ui.config.lanes.front().label = "Operator profile";
    ui.config.discover_lora = true; ui.config.automatic_decode = true; ui.decode_enabled = true; ui.spectrum_only = false;
    ui.start(engine, false); require(!ui.notice_error, ui.notice); measurements(engine);
    auto live = engine.snapshot();
    require(!live.config.discover_lora && !live.config.automatic_decode && live.config.lanes.empty() &&
        !ui.config.discover_lora && ui.config.lanes.empty() && !ui.decode_enabled && ui.spectrum_only,
        "Start enforces spectrum only even if an old caller supplies manual or automatic LoRa setup");
    engine.stop();
    ui.spectrum_only = false; ui.decode_enabled = false; ui.config.discover_lora = false;
    ui.start(engine, false); require(!ui.notice_error, ui.notice); measurements(engine);
    require(engine.snapshot().config.lanes.empty() && ui.config.lanes.empty(),
        "Resume keeps manual decoder profiles disabled");
    engine.stop();
    ui.decode_enabled = true;
    ui.start(engine, false); require(!ui.notice_error, ui.notice); measurements(engine);
    require(engine.snapshot().config.lanes.empty() && !ui.decode_enabled,
        "An attempted stale classification toggle cannot enable desktop packet processing");
    engine.stop();
    ui.config.center_hz = 868750000; ui.config.tuning_offset_hz = 900;
    ui.start(engine, false); require(!ui.notice_error, ui.notice); measurements(engine);
    require(engine.snapshot().config.lanes.empty() && ui.config.lanes.empty(),
        "Changing survey frequency keeps the effective receiver spectrum only");
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

void rtl_receiver_controls(Canvas& canvas) {
    Engine engine; DesktopState ui; ui.passive_smoke=true;
    ui.select_receiver(2); ui.config.center_hz=906875000;
    Snapshot snapshot; snapshot.rtl_sdr_available=true;
    const auto draw=[&] { reception_control(engine,ui,snapshot); receiver_controls(engine,ui,snapshot); };
    canvas.frame(draw);const auto rtl=canvas.frame(draw);
    for(const auto* text:{"RTL-SDR / USB","Tuner gain","Automatic tuner gain","2 MS/s","906.125 - 907.625 MHz",
            "Only this range is monitored continuously."})contains(rtl,text);
    for(const auto* text:{"LNA gain","VGA gain","RF amplifier","16 MS/s"})
        require(rtl.find(text)==std::string::npos,"RTL controls do not offer HackRF gain stages or unsupported wideband rates");
    require(!engine.snapshot().running&&engine.gps_connection_status().state==GpsConnectionState::Disconnected,
        "Rendering RTL controls starts neither radio nor GPS");
    snapshot.rtl_sdr_available=false;
    contains(canvas.frame(draw),"RTL-SDR support is unavailable in this build.");
    snapshot.historical=true;snapshot.config=ui.config;snapshot.config.rtl_gain_tenths_db=297;
    ui.select_receiver(1);
    const auto saved=canvas.frame(draw);
    contains(saved,"RTL-SDR / USB");contains(saved,"Applied tuner gain: 29.7 dB");
    require(saved.find("RF amplifier")==std::string::npos,
        "Historical RTL recordings show their own receiver controls even when the next session selects HackRF");
    require(ui.source==1&&ui.config.hardware_receiver==HardwareReceiver::HackRf,
        "Displaying historical RTL acquisition does not mutate the next receiver setup");
}

void concentrator_controls_and_measurements(Canvas& canvas) {
    Engine engine; DesktopState ui; ui.passive_smoke=true;
    ui.select_receiver(3);
    require(ui.config.center_hz==915000000 && ui.config.survey_span_hz==26000000 &&
        ui.config.hardware_receiver==HardwareReceiver::Rak5146 && !ui.config.amplifier,
        "RAK selects its swept US915 range without an SDR amplifier");
    Snapshot snapshot; snapshot.rak5146_available=true;
    const auto draw=[&] { reception_control(engine,ui,snapshot); receiver_controls(engine,ui,snapshot); };
    canvas.frame(draw); const auto controls=canvas.frame(draw);
    for(const auto* text:{"RAK5146 USB/LBT","Configure RAK boards...","902.000 - 928.000 MHz","visited sequentially"}) contains(controls,text);
    for(const auto* text:{"LNA gain","VGA gain","RF amplifier","Sample rate","Tuner gain"})
        require(controls.find(text)==std::string::npos,"Concentrator controls hide irrelevant SDR settings");
    ui.concentrator_inventory_loaded=true;
    const auto setup=canvas.frame([&] { concentrator_settings(engine,ui,snapshot); });
    for(const auto* text:{"Number of boards","Choose USB concentrator...","generic STM32 identity",
            "Scan RF energy across the survey range", "Scan step / kHz"}) contains(setup,text);
    for(const auto* text:{"Receive LoRa packets", "Packet frequency / MHz", "Packet bandwidth", "Spreading factor",
            "Sync word", "Apply preset", "Configured key records", "Configure authorized keys"})
        require(setup.find(text) == std::string::npos, "RAK spectrum setup hides packet profiles and classification keys");
    require(!engine.snapshot().running && engine.gps_connection_status().state==GpsConnectionState::Disconnected,
        "Concentrator UI rendering opens neither USB nor GPS");
    ui.config.concentrators.boards[0].frequency_hz=907000000;
    const auto custom=canvas.frame([&] { concentrator_settings(engine,ui,snapshot); });
    require(custom.find("LongFast") == std::string::npos, "Hidden packet profile is not shown in scan setup");
    require(ui.config.concentrators.boards[0].frequency_hz==907000000,
        "Changing a manual frequency preserves its value and does not change the modem preset");
    auto applied=ui.config; applied.sample_rate=0; applied.discover_lora=false; applied.lanes.clear();
    // The low-level metadata formatter remains compatible with old recordings.
    applied.concentrators.decode_enabled=true; applied.concentrators.boards[0].packets_enabled=true;
    applied.concentrators.boards[0].device_path="fixture-private-path";
    applied.concentrators.boards[0].device_id="fixture-private-id";
    auto log=desktop_acquisition_log(applied,0,90,1700000090);
    for(const auto* text:{"boards=1","scan_enabled=1","samples_per_scan=2000","decode_enabled=1",
            "board=1 packets_enabled=1 frequency_hz=907000000 bandwidth_hz=250000 sf=11 sync_word=0x2b",
            "decoding_scope=configured_hardware_profiles"}) contains(log,text);
    for(const auto* text:{"sample_rate=","lna_gain_db=","fixture-private","paused_spectrum_only"})
        require(log.find(text)==std::string::npos,"RAK startup log excludes irrelevant SDR values and private USB identities");
    applied.concentrators.decode_enabled=false;
    contains(desktop_acquisition_log(applied,0,90,0),"decoding_scope=disabled_packet_metadata_only");
    applied.concentrators.boards[0].packets_enabled=false;
    contains(desktop_acquisition_log(applied,0,90,0),"decoding_scope=paused_spectrum_only");
    applied.hardware_receiver=HardwareReceiver::RtlSdr; applied.sample_rate=2000000; applied.rtl_gain_tenths_db=297;
    log=desktop_acquisition_log(applied,0,90,0);
    contains(log,"sample_rate=2000000"); contains(log,"rtl_gain_tenths_db=297");
    require(log.find("boards=")==std::string::npos,"SDR startup logging preserves applied receiver settings");
    snapshot.config=ui.config; snapshot.config.sample_rate=0; snapshot.session_id="synthetic-rak-ui";
    snapshot.config.session_path="fixture-only-not-opened.sqlite";
    snapshot.historical=true; snapshot.concentrator_scans=9; snapshot.concentrator_rssi_samples=18000;
    ConcentratorScan scan; scan.frequency_hz=906875000; scan.utc_end_seconds=1700000001.25;
    scan.elapsed_start_seconds=1; scan.elapsed_end_seconds=1.025; scan.counts[0]=500; scan.counts[32]=1500;
    snapshot.recent_concentrator_scans={scan};
    const auto analysis=canvas.frame([&] { compact_analysis_tab(engine,ui,snapshot); });
    for(const auto* text:{"Generate analysis report...","Export report...","Sampled RF energy","234.3 kHz",
            "sampled exceedance fractions","25.000","906.875000"}) contains(analysis,text);
    queue_analysis(ui,snapshot,false);
    require(!ui.analysis_busy(),"Concentrator Analyze and historical startup never queue an FFT occupancy query");
    ui.show_analysis_details=true;
    canvas.full_frame([&] { render(engine,ui,snapshot); });
    ui.show_analysis_details=false;
    require(analysis.find("Busy time / selected range")==std::string::npos,"Concentrator scans never masquerade as FFT busy-time observations");
    prepare_report_export(ui,snapshot); ui.export_kind=1;
    const auto report=canvas.frame([&] { report_export_panel(engine,ui,snapshot); });
    contains(report,"Scan readings over time"); contains(report,"One row per scan");
    require(report.find("Time bucket / seconds")==std::string::npos,"RAK scan reports do not promise interpolated time buckets");
    ui.export_kind=3; ui.export_path="synthetic.csv";
    require(!report_export_block_reason(ui,snapshot).empty(),"Unsupported RAK waveform reports are blocked");
    Reception rx; rx.utc_seconds=1700000001.25; rx.concentrator=ConcentratorPacketMetadata{0,-72,12345};
    const auto detail=canvas.frame([&] { reception_detail(rx); });
    contains(detail,"not authenticated");
    require(detail.find("AUTHORIZED SCHEMA FIELDS") == std::string::npos && detail.find("Reported origin") == std::string::npos, "Concentrator details exclude semantic message fields");
    contains(detail,"RSSI -72.0 dBm (uncalibrated)"); contains(detail,"Board-local timestamp: 12345 us");
    require(detail.find("Frequency error 0 Hz")==std::string::npos,"Unavailable concentrator frequency error is not reported as zero");
    snapshot.historical=false; snapshot.rak5146_available=false;
    contains(canvas.frame(draw),"RAK5146 support is unavailable in this build.");
    ui.select_receiver(1);
    require(ui.config.survey_span_hz<=ui.config.sample_rate*4/5,"Returning from RAK restores a valid SDR width");
}

void concentrator_selector_mouse_input(Canvas& canvas) {
    Engine engine; DesktopState ui; ui.passive_smoke = true;
    ui.select_receiver(3); ui.show_settings = true; ui.settings_page = 6;
    ui.concentrator_inventory_loaded = true;
    const ConcentratorDevice candidate{"/dev/cu.fixture-rak", "Synthetic STM32 USB candidate (0483:5740)",
        "usb:0483:5740:serial:66697874757265"};
    ui.concentrator_devices.devices = {candidate};
    Snapshot snapshot; snapshot.rak5146_available = true;
    const auto draw = [&] { canvas.full_frame([&] { render(engine, ui, snapshot); }); };
    const auto key = [&](ImGuiKey value) {
        ImGui::GetIO().AddKeyEvent(value, true); draw();
        ImGui::GetIO().AddKeyEvent(value, false); draw();
    };
    const auto navigate = [&](ImGuiWindow* window, ImGuiID id) {
        ImGui::FocusWindow(window);
        for (unsigned attempt = 0; attempt < 64 && ImGui::GetCurrentContext()->NavId != id; ++attempt)
            key(ImGuiKey_Tab);
        require(ImGui::GetCurrentContext()->NavId == id, "Keyboard navigation reaches the actual RAK selector control");
    };
    const auto nav_center = [](ImGuiWindow* window) {
        const auto rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
        return ImVec2{(rect.Min.x + rect.Max.x) * .5f, (rect.Min.y + rect.Max.y) * .5f};
    };
    const auto click = [&](ImVec2 point) {
        ImGui::GetIO().AddMousePosEvent(point.x, point.y); draw();
        ImGui::GetIO().AddMouseButtonEvent(0, true); draw();
        ImGui::GetIO().AddMouseButtonEvent(0, false); draw(); draw();
    };
    draw(); draw();
    ImGuiWindow* page = nullptr;
    for (auto* window : ImGui::GetCurrentContext()->Windows)
        if (window->Active && std::string(window->Name).find("settingsPage") != std::string::npos) page = window;
    require(page && page->Active, "RAK settings render inside the real Settings child window");
    const auto board_id = ImHashStr("##boardDevice", 0, page->GetID(0));
    navigate(page, board_id); const auto board_point = nav_center(page); click(board_point);
    auto* context = ImGui::GetCurrentContext();
    require(!context->OpenPopupStack.empty(), "Mouse click opens Board 1 USB selector from Settings child");
    auto* popup = context->OpenPopupStack.back().Window;
    require(popup && popup->Active && !popup->Hidden, "Board candidate popup is visible in the full desktop frame");
    navigate(popup, popup->GetID("###candidate0")); click(nav_center(popup));
    require(ui.config.concentrators.boards[0].device_id == candidate.stable_id &&
        ui.config.concentrators.boards[0].device_path == candidate.path && context->OpenPopupStack.empty(),
        "Mouse-selecting a unique USB candidate saves its identity and path and closes the popup");
    require(!engine.snapshot().running && engine.snapshot().session_id.empty() &&
        engine.gps_connection_status().state == GpsConnectionState::Disconnected && !ui.preferences_active,
        "Selecting a fixture concentrator opens no USB or GPS and touches no ordinary preferences");

    auto& board = ui.config.concentrators.boards[0]; board.device_id.clear(); board.device_path.clear();
    const auto setup = [&] { return canvas.frame([&] { concentrator_settings(engine, ui, snapshot); }); };
    const auto available = setup(); contains(available, "Select this board"); contains(available, candidate.label.c_str()); contains(available, candidate.path.c_str());
    require(board.device_id.empty() && board.device_path.empty(), "Showing a single USB candidate never selects it automatically");
    draw(); draw();
    navigate(page, ImHashStr("Select this board", 0, page->GetID(0)));
    const auto shortcut_point = nav_center(page); click(shortcut_point);
    require(board.device_id == candidate.stable_id && board.device_path == candidate.path &&
        context->OpenPopupStack.empty() && !engine.snapshot().running && engine.snapshot().session_id.empty() &&
        engine.gps_connection_status().state == GpsConnectionState::Disconnected,
        "Explicit mouse click selects the visible single candidate without a popup or hardware connection");

    snapshot.session_id = "synthetic-paused-rak"; snapshot.config = ui.config;
    const auto locked = canvas.frame([&] { concentrator_settings(engine, ui, snapshot); });
    contains(locked, "Choose New to change concentrator boards or scan settings.");
    contains(locked, "USB candidates found: 1"); contains(locked, "Refresh USB candidates");
    draw(); draw(); click(board_point);
    require(context->OpenPopupStack.empty() && ui.config.concentrators.boards[0].device_id == candidate.stable_id,
        "Paused session keeps its concentrator setup locked instead of erasing measurements");
    board.device_id.clear(); board.device_path.clear(); draw(); draw(); click(shortcut_point);
    require(board.device_id.empty() && board.device_path.empty(), "Single-candidate shortcut obeys the existing session configuration lock");
    snapshot.session_id.clear();
    const ConcentratorDevice second{"/dev/cu.fixture-rak-two", "Second synthetic candidate", "usb:0483:5740:serial:7365636f6e64"};
    ui.concentrator_devices.devices = {candidate, second};
    require(setup().find("Select this board") == std::string::npos, "Multiple eligible boards require the explicit candidate list");
    ui.concentrator_devices.devices = {candidate, candidate};
    require(setup().find("Select this board") == std::string::npos, "Ambiguous duplicate USB identities never receive a shortcut");
    ui.concentrator_devices.devices = {candidate}; ui.config.concentrators.boards.resize(2);
    ui.config.concentrators.boards[1].device_id = candidate.stable_id; ui.config.concentrators.boards[1].device_path = candidate.path;
    require(setup().find("Select this board") == std::string::npos, "A board assigned to another slot is excluded from the shortcut");
    ui.config.concentrators.boards.resize(1);
    ui.config.concentrators.boards[0].frequency_hz = 907125000;
    ui.config.concentrators.boards[0].device_id = candidate.stable_id;
    ui.config.concentrators.boards[0].device_path = candidate.path;
    if (desktop_lora_enabled) {
    draw(); draw();
    navigate(page, ImHashStr("Apply preset", 0, page->GetID(0))); click(nav_center(page));
    require(!context->OpenPopupStack.empty(), "RAK modem preset picker opens without device access");
    popup = context->OpenPopupStack.back().Window;
    navigate(popup, popup->GetID("MediumFast")); click(nav_center(popup));
    require(ui.config.concentrators.boards[0].frequency_hz == 907125000 &&
        ui.config.concentrators.boards[0].bandwidth_hz == 250000 &&
        ui.config.concentrators.boards[0].spreading_factor == 9 && ui.config.concentrators.boards[0].sync_word == 0x2b &&
        context->OpenPopupStack.empty(), "RAK catalog preset changes modulation without assuming a regional frequency");
    }
    ui.show_settings = false; ImGui::GetIO().AddMousePosEvent(-1000, -1000); draw();
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
        ImGuiWindow* main=nullptr;
        for(auto* window:ImGui::GetCurrentContext()->Windows)
            if(std::string(window->Name).find("receiverSidebar")!=std::string::npos && window->Active) main=window;
        require(main && main->Active, "Sidebar contains the session controls");
        const auto target = main->GetID("Open...");
        navigate(main, target);
        const auto rect = ImGui::WindowRectRelToAbs(main, main->NavRectRel[ImGuiNavLayer_Main]);
        const ImVec2 point{(rect.Min.x + rect.Max.x) / 2, (rect.Min.y + rect.Max.y) / 2};
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

void draggable_live_panels(Canvas& canvas) {
    Engine engine; DesktopState ui;
    const auto draw=[&] { canvas.full_frame([&] { render(engine,ui,engine.snapshot()); }); };
    draw();draw();
    const float spectrum=ui.spectrum_height, waterfall=ui.waterfall_height;
    const auto drag=[&](ImVec2 point,float dy) {
        ImGui::GetIO().AddMousePosEvent(point.x+100,point.y+6);draw();
        ImGui::GetIO().AddMouseButtonEvent(0,true);draw();
        ImGui::GetIO().AddMousePosEvent(point.x+100,point.y+6+dy);draw();
        ImGui::GetIO().AddMouseButtonEvent(0,false);draw();draw();
    };
    drag(ui.spectrum_grabber,-30);
    require(ui.spectrum_height<=spectrum-25 && ui.waterfall_height==waterfall,
        "Dragging spectrum divider shrinks only spectrum and frees results space");
    const float reduced=ui.spectrum_height;
    drag(ui.waterfall_grabber,-60);
    require(ui.waterfall_height<=waterfall-55 && ui.spectrum_height==reduced,
        "Waterfall divider independently frees space for the result tables");
    drag(ui.waterfall_grabber,10000);
    require(ui.waterfall_height<700,"Divider cannot drag results outside the viewport");
    ImGui::GetIO().DisplaySize={1000,680};ui.ui_scale=1.5f;draw();draw();
    require(ui.spectrum_height>=60 && ui.waterfall_height>=70 && ui.waterfall_grabber.y<650,
        "Both dividers remain reachable at minimum window size and large UI scale");
    ImGui::GetIO().DisplaySize={1440,960};ui.ui_scale=1;draw();
    require(engine.snapshot().session_id.empty()&&!engine.snapshot().running,
        "Layout interactions never create a survey or open hardware");
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
        preset_catalog_and_automatic_status(canvas);
        persisted_setup_and_effective_modes(fixture);
        rtl_receiver_controls(canvas);
        concentrator_controls_and_measurements(canvas);
        concentrator_selector_mouse_input(canvas);
        save_new_and_historical(canvas, fixture);
        unrecorded_and_pending_feedback(canvas);
        asynchronous_analysis_preserves_selection(canvas, fixture);
        scaled_settings_scopes(canvas);
        toolbar_open_displays_chooser(canvas, fixture);
        final_analysis_refresh_after_stop(canvas, fixture);
        draggable_live_panels(canvas);
        std::cout << "Desktop workflow checks passed; synthetic input only, no windows or devices\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "Desktop workflow checks failed: " << error.what() << '\n'; return 1; }
}
