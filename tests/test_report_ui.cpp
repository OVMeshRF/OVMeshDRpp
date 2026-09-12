// SPDX-License-Identifier: GPL-3.0-or-later
// Actual ImGui controls with synthetic metadata and explicit local preference
// fixtures. No OS window, device discovery, RF, GPS, network or user profile.
#include "../src/ui.cpp"
#include <imgui_internal.h>
#include <iostream>
#include <limits>
#include "../src/storage.hpp"
#include <fstream>

namespace {
using namespace ovmesh;
namespace fs = std::filesystem;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void contains(const std::string& text, const char* expected) {
    if (text.find(expected) == std::string::npos)
        throw std::runtime_error(std::string("Missing rendered text: ") + expected + "\n" + text);
}
struct Fixture {
    fs::path directory = fs::current_path() / ("report-ui-fixture-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { require(fs::create_directory(directory), "Create isolated report UI fixture"); }
    ~Fixture() { std::error_code error; fs::remove_all(directory, error); }
    std::string path(const char* name) const { return path_utf8(directory / name); }
};
class Canvas {
public:
    Canvas() {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.DisplaySize = {1200, 1050}; io.DeltaTime = 1.f / 60;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.AddFocusEvent(true);
        unsigned char* pixels; int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        require(pixels && width > 0 && height > 0, "Create in-memory font atlas");
    }
    ~Canvas() { ImGui::DestroyContext(); }
    template<class Render> std::string frame(Render render) {
        ImGui::NewFrame(); ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Report fixture", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
        ImGui::LogToBuffer(); render();
        const std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish(); ImGui::End(); ImGui::Render();
        const auto* context = ImGui::GetCurrentContext();
        require(context->CurrentWindowStack.empty() && context->BeginPopupStack.empty() && !context->CurrentTable,
            "Report UI balances ImGui scopes");
        const auto* draw = ImGui::GetDrawData();
        require(draw && draw->Valid && draw->TotalVtxCount > 0, "Report UI renders actual geometry");
        for (int i = 0; i < draw->CmdListsCount; ++i)
            for (const auto& vertex : draw->CmdLists[i]->VtxBuffer)
                require(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y), "Report geometry remains finite");
        return text;
    }
    template<class Render> void click(Render render, ImVec2 point) {
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(point.x, point.y); frame(render);
        io.AddMouseButtonEvent(0, true); frame(render);
        io.AddMouseButtonEvent(0, false); frame(render);
    }
    template<class Render> void key(Render render, ImGuiKey key) {
        ImGui::GetIO().AddKeyEvent(key, true); frame(render);
        ImGui::GetIO().AddKeyEvent(key, false); frame(render);
    }
};

void recording_mode(Canvas& canvas, const Fixture& fixture) {
    DesktopState ui; ui.initialize_preferences(fixture.path("profile"), false);
    require(ui.preferences_ready && ui.config.compact_recording, "New desktop defaults to compact recording");
    require(ui.gps_devices.devices.empty() && ui.config.session_path.empty(), "Setup skips device discovery and recording");
    Snapshot snapshot; ImVec2 combo_point;
    const auto render = [&] {
        const auto position = ImGui::GetCursorScreenPos();
        combo_point = {position.x + 100, position.y + ImGui::GetFrameHeight() * .5f};
        ImGui::SetNextItemWidth(300);
        recording_mode_control(ui, snapshot);
    };
    canvas.frame(render);
    auto text = canvas.frame(render);
    contains(text, "Compact (default)"); contains(text, "new recordings only");
    contains(text, "original GPS fixes");
    snapshot.running = true;
    canvas.click(render, combo_point);
    require(ui.config.compact_recording && ImGui::GetCurrentContext()->OpenPopupStack.empty(),
        "Running survey disables the actual recording-mode selector");
    snapshot.running = false;
    canvas.click(render, combo_point);
    require(!ImGui::GetCurrentContext()->OpenPopupStack.empty(), "Stopped survey opens the actual recording-mode selector");
    canvas.key(render, ImGuiKey_DownArrow);
    canvas.key(render, ImGuiKey_Enter);
    require(!ui.config.compact_recording && !load_preferences(ui.preference_locations).compact_recording,
        "Selecting Detailed through ImGui changes and persists the next-recording mode");
    contains(canvas.frame(render), "20 ms power measurements");
    DesktopState reopened; reopened.initialize_preferences(fixture.path("profile"), false);
    require(!reopened.config.compact_recording, "Detailed recording survives ordinary setup reload");
    reopened.config.compact_recording = true; reopened.persist_preferences();
    DesktopState launch; launch.config.compact_recording = false;
    launch.initialize_preferences(fixture.path("profile"), false);
    require(!launch.config.compact_recording, "Explicit detailed launch overrides saved Compact preference");
    require(launch.config.session_path.empty() && !launch.managed_started, "Preference loading does not start a survey");
}

Snapshot saved_snapshot(const Fixture& fixture) {
    Snapshot snapshot; snapshot.session_id = "generated-report-session";
    snapshot.config.session_title = "Generated report survey";
    snapshot.config.session_path = fixture.path("synthetic-source.sqlite");
    snapshot.historical = true;
    return snapshot;
}
void selection_and_privacy(Canvas& canvas, const Fixture& fixture) {
    Engine engine; DesktopState ui; auto snapshot = saved_snapshot(fixture);
    ui.analysis_loaded = true; ui.analysis_session = snapshot.session_id;
    ui.analyzed_query.lower_hz = 906750000; ui.analyzed_query.upper_hz = 907000000;
    ui.analyzed_query.elapsed_start = 3; ui.analyzed_query.elapsed_end = 123;
    ui.analyzed_query.geographic_filter = true;
    ui.analyzed_query.south = 39; ui.analyzed_query.north = 40;
    ui.analyzed_query.west = -81; ui.analyzed_query.east = -80;
    ui.export_options.include_receiver_positions = true;
    prepare_report_export(ui, snapshot);
    require(ui.export_kind == 0 && ui.export_time_bucket_seconds == 60 && ui.export_geographic_cell_m == 100,
        "Export defaults to frequency summary, 60-second time groups and 100-meter geographic cells");
    require(!ui.export_options.include_receiver_positions &&
        !ui.export_options.include_provenance && ui.export_options.coordinate_decimals == 3,
        "Each export starts with the established private defaults");
    const auto render = [&] { report_export_panel(engine, ui, snapshot); };
    canvas.frame(render); const auto text = canvas.frame(render);
    for (const auto* expected : {"Frequency summary (CSV)", "latest completed analysis selection", "906.750000 - 907.000000",
        "3.000 - 123.000", "Receiver area: S 39.000000", "Choose an export location", "Write report"}) contains(text, expected);
    ui.analyzed_query.lower_hz = 800000000;
    require(selected_report_options(ui).query.lower_hz == 906750000, "An open export preserves its explicitly shown selection");
    require(!report_export_block_reason(ui, snapshot).empty(), "Missing export path blocks writing");
    ui.export_path = fixture.path("summary.csv");
    require(report_export_block_reason(ui, snapshot).empty(), "Saved selected-frequency CSV report is ready");
    ui.export_path = fixture.path("summary.geojson");
    contains(report_export_block_reason(ui, snapshot), ".csv filename");
    ui.export_path = fixture.path("summary.csv");

    require(text.find("Include authorized decoded content") == std::string::npos, "Reports do not offer a content export opt-in");
    ui.export_kind = 3;
    const auto spectrum_report = canvas.frame(render);
    require(ui.export_kind == 0 && spectrum_report.find("Waveform observations") == std::string::npos,
        "The spectrum-only desktop cannot select a stale waveform report; frequency reports remain available");
    ui.export_kind = 1;
    contains(canvas.frame(render), "Time bucket / seconds");
    require(selected_report_options(ui).query.time_bucket_seconds == 60, "Report bucket default is independent of display coarsening");
    ui.export_time_bucket_seconds = .0001;
    require(!report_export_block_reason(ui, snapshot).empty(), "Unsupported time precision is blocked");
    ui.export_time_bucket_seconds = 60;
    ui.export_kind = 2;
    contains(report_export_block_reason(ui, snapshot), "requires Include receiver GPS coordinates");
    contains(canvas.frame(render), "default is 100 m");
    ui.export_options.include_receiver_positions = true;
    contains(canvas.frame(render), "does not change original GPS fixes");
    require(report_export_block_reason(ui, snapshot).empty(), "Geographic report requires explicit receiver-coordinate opt-in");
    ui.export_geographic_cell_m = std::numeric_limits<double>::quiet_NaN();
    require(!report_export_block_reason(ui, snapshot).empty(), "Nonfinite cell size is blocked");
    ui.export_geographic_cell_m = 100;
    ui.export_kind = 4; ui.export_options.include_receiver_positions = false;
    contains(report_export_block_reason(ui, snapshot), "requires Include receiver GPS coordinates");
    ui.export_kind = 5; ui.export_path = fixture.path("archive.geojson");
    require(report_export_block_reason(ui, snapshot).empty(), "Explicit detailed archive still supports GeoJSON");
    ImVec2 write_point;
    const auto archive = canvas.frame([&] {
        render();
        const auto low = ImGui::GetItemRectMin(), high = ImGui::GetItemRectMax();
        write_point = {(low.x + high.x) * .5f, (low.y + high.y) * .5f};
    });
    contains(archive, "whole saved session"); contains(archive, "filters do not restrict this archive");
    contains(archive, "compact recordings cannot recover original 20 ms");
    require(archive.find("Frequency: 906.750000") == std::string::npos, "Archive does not present the analysis selection as its scope");
    snapshot.running = true;
    contains(canvas.frame(render), "Stop reception before exporting");
    require(!report_export_block_reason(ui, snapshot).empty(), "Running survey blocks archive and report export");
    ui.notice = "unchanged";
    canvas.click(render, write_point);
    require(ui.notice == "unchanged", "The actual export button cannot dispatch while reception is running");
    snapshot.running = false;
    snapshot.session_id = "another-generated-session";
    contains(canvas.frame(render), "selected survey changed");
    require(!report_export_block_reason(ui, snapshot).empty(), "An open export never applies one session's selection to another survey");
    snapshot.session_id = ui.export_session_id;
    snapshot.config.session_path.clear();
    contains(canvas.frame(render), "Open a saved survey or record a session");
    require(!engine.snapshot().running && !fs::exists(fixture.directory / "summary.csv") &&
        !fs::exists(fixture.directory / "archive.geojson"), "Rendering alone creates no exports or receiver activity");
    ui.analysis_session = "other-session";
    prepare_report_export(ui, snapshot);
    require(!ui.export_from_analysis && ui.export_query.lower_hz == 0 && !ui.export_query.geographic_filter,
        "Another session's analysis never silently filters a new report");
}

void aggregated_power(Canvas& canvas) {
    require(quality_text(SurveyPowerAggregated) == "power aggregated", "Quality bit512 has a human-readable label");
    const auto explanation = quality_explanation(SurveyPowerAggregated | SurveyBoundary);
    contains(explanation, "exact 20 ms power detail cannot be reconstructed");
    contains(explanation, "activity timing and original GPS fixes retain their precision");
    const auto rendered = canvas.frame([&] {
        ImGui::TextWrapped("Quality: %s", quality_text(SurveyPowerAggregated).c_str());
        ImGui::TextWrapped("%s", explanation.c_str());
    });
    contains(rendered, "power aggregated"); contains(rendered, "Power aggregated");
}
void narrative_controls(Canvas& canvas,const Fixture& fixture) {
    Engine engine;DesktopState ui;auto snapshot=saved_snapshot(fixture);
    prepare_report_export(ui,snapshot);ui.export_kind=6;
    const auto render=[&]{report_export_panel(engine,ui,snapshot);};
    require(selected_report_options(ui).kind == ReportKind::Analysis, "Narrative UI choice maps explicitly to the HTML report kind");
    const auto page=canvas.frame(render);
    contains(page,"Analysis report (HTML)");contains(page,"No cloud service is used");
    require(page.find("Include authorized decoded content")==std::string::npos,"Narrative does not offer a misleading payload opt-in");
    contains(page,"Preview");
    require(report_export_block_reason(ui,snapshot,true).empty(),"Preview needs no selected output path");
    const auto preview=analysis_preview_path(ui,snapshot);
    require(fs::path(preview).parent_path()==fixture.directory && fs::path(preview).extension()==".html" && !fs::exists(preview),"Preview reserves no file and stays in the local session directory without an initialized profile");
    ui.export_path=fixture.path("analysis.csv");require(!report_export_block_reason(ui,snapshot).empty(),"Wrong HTML suffix blocked");
    ui.export_path=fixture.path("analysis.html");require(report_export_block_reason(ui,snapshot).empty(),"HTML suffix accepted");
    begin_file_picker(ui,FilePickerPurpose::Export,"",snapshot.config.session_path);
    require(std::string(ui.file_picker.filename.data())=="survey-analysis.html","HTML chooser suggests usable filename");
    snapshot.running=true;require(!report_export_block_reason(ui,snapshot).empty() && !report_export_block_reason(ui,snapshot,true).empty(),"Preview and export require a stopped consistent session");
    ui.export_time_bucket_seconds=0;snapshot.running=false;
    require(!report_export_block_reason(ui,snapshot).empty(),"Narrative grouping validated before write");
    require(!engine.snapshot().running&&!fs::exists(fixture.path("analysis.html")),"Preview does not open devices or create report");
}

void report_open_flow(const Fixture& fixture) {
    const auto source=fixture.path("open-flow.sqlite");
    { SessionStore store; ReceiverConfig config;config.lanes.clear();config.discover_lora=false;
      store.create(source,config,"synthetic-preview");
      SpectrumTile tile;tile.id=1;tile.frame_count=4;tile.first_sample=0;tile.end_sample=16384;
      tile.elapsed_end_seconds=double(tile.end_sample)/config.sample_rate;
      tile.utc_start_seconds=1700000000;tile.utc_end_seconds=1700000000+tile.elapsed_end_seconds;
      tile.first_center_hz=config.center_hz;tile.bin_width_hz=double(config.sample_rate)/4096;
      tile.mean_dbfs={-80,-80};tile.peak_dbfs={-40,-40};tile.activity={1,0,1,0};tile.background_dbfs=-100;store.append(tile);
      Snapshot snapshot;snapshot.config=config;snapshot.elapsed_seconds=tile.elapsed_end_seconds;store.update(snapshot,true); }
    Engine engine;std::string error;require(engine.open_session(source,error),"Load report fixture without hardware");
    DesktopState ui;prepare_report_export(ui,engine.snapshot());ui.export_kind=6;
    const auto path=analysis_preview_path(ui,engine.snapshot());bool opened=false;
    start_analysis_report(engine,ui,path,[&](const std::string& value){
        require(value==path && fs::is_regular_file(value),"Browser receives the generated file only after success");opened=true;
    });
    ui.operation.wait();ui.finish_operation();
    require(opened && !ui.show_export,"Successful preview opens once and closes export dialog");
    opened=false;start_analysis_report(engine,ui,path,[&](const auto&){opened=true;});
    ui.operation.wait();ui.finish_operation();
    require(!opened,"Exclusive-write failure never opens or overwrites a previous report");
    const auto second=analysis_preview_path(ui,engine.snapshot());
    start_analysis_report(engine,ui,second,[](const auto&){throw std::runtime_error("No default browser");});
    ui.operation.wait();ui.finish_operation();
    require(fs::is_regular_file(second) && ui.notice.find("Browser opening failed")!=std::string::npos,"Browser launch failure preserves the generated file and reports its location");
    for(const auto* bad:{"https://example.invalid/report.html","relative.html","/missing-report.html"}) {
        bool rejected=false;try{open_local_report(bad);}catch(const std::exception&){rejected=true;}
        require(rejected,"Opener rejects URL, relative, or absent files before launching");
    }
}

void compact_analysis_keeps_frequency_context(Canvas& canvas, const Fixture& fixture) {
    Engine engine; DesktopState ui; Snapshot live;
    live.session_id = "memory-only-measurements"; live.running = true;
    live.measurement_seconds = 10;
    live.frequencies = {{906875000,250000,-72,-32,10,1},{908750000,500000,-80,-40,10,.5}};
    const auto memory = canvas.frame([&] { compact_analysis_tab(engine, ui, live); });
    contains(memory, "Live frequency measurements / not recording");
    contains(memory, "By frequency"); contains(memory, "Saved history is required");
    require(!ui.pending_analysis && !ui.analysis_loaded && !engine.snapshot().running,
        "Live frequency display remains available without a stored survey or an implicit query");

    auto saved = saved_snapshot(fixture); saved.frequencies = live.frequencies;
    ui.analysis_loaded = true; ui.analysis_session = saved.session_id;
    ui.last_analysis_refresh = ImGui::GetTime();
    ui.analysis.covered_lower_hz = 906750000; ui.analysis.covered_upper_hz = 907000000;
    ui.analysis.resolved_elapsed_start = 5; ui.analysis.resolved_elapsed_end = 15;
    ui.analysis.observed_seconds = 10; ui.analysis.busy_seconds = 1;
    SurveyObservation observation; observation.elapsed_start = 5; observation.elapsed_end = 15;
    observation.observed_seconds = 10; observation.busy_seconds = 1;
    ui.analysis.observations.push_back(observation);
    ui.analysis_view = 1;
    const auto selected = canvas.frame([&] { compact_analysis_tab(engine, ui, saved); });
    for (const char* text : {"906.750000 - 907.000000 MHz", "Width: 250.000 kHz", "Busy time / selected range",
            "10.000%", "Elapsed interval / s", "Observed s", "Busy s", "Receiver position"}) contains(selected, text);
    require(!ui.pending_analysis && !fs::exists(fs::path(saved.config.session_path)),
        "Rendering an already selected analysis does not open or create a source database");
}
} // namespace

int main() {
    try {
        Fixture fixture; Canvas canvas;
        recording_mode(canvas, fixture);
        selection_and_privacy(canvas, fixture);
        aggregated_power(canvas);
        narrative_controls(canvas,fixture);
        report_open_flow(fixture);
        compact_analysis_keeps_frequency_context(canvas, fixture);
        std::cout << "Compact recording and report UI checks passed; no hardware or ordinary profile access\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Report UI checks failed: " << error.what() << '\n'; return 1;
    }
}
