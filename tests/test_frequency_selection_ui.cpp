// SPDX-License-Identifier: GPL-3.0-or-later
// Actual ImGui gestures and saved synthetic analysis. No OS windows or devices.
#include "../src/ui.cpp"
#include "../src/storage.hpp"
#include <imgui_internal.h>
#include <fstream>
#include <iostream>

namespace {
using namespace ovmesh;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

class SelectionUi {
public:
    FrequencyChartSelection selection;
    std::vector<FrequencySummary> bins;
    float left = 0, right = 0, top = 0, bottom = 0;
    size_t outline_vertices = 0;
    SelectionUi() {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.LogFilename = nullptr; io.DeltaTime = 1.0f / 60;
        io.DisplaySize = {700, 400}; io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr; int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        for (size_t i = 0; i < 100; ++i) bins.push_back({906500000 + i * 10000, 10000, -80, -40, 60, 6});
        frame(); frame();
    }
    ~SelectionUi() { ImGui::DestroyContext(); }
    std::optional<FrequencyRange> frame(bool enabled = true) {
        ImGui::NewFrame(); ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Frequency selection fixture", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
        const auto origin = ImGui::GetCursorScreenPos();
        left = origin.x + std::max(54.0f, ImGui::CalcTextSize("0.001%").x + 10);
        right = origin.x + std::max(200.0f, ImGui::GetContentRegionAvail().x) - 12;
        top = origin.y + 18; bottom = origin.y + 210 - 30;
        occupancy_chart(bins, 210, OccupancyScale::RevealLowActivity, 0, 0, enabled ? &selection : nullptr);
        ImGui::End(); ImGui::Render();
        outline_vertices = 0;
        for (int n = 0; n < ImGui::GetDrawData()->CmdListsCount; ++n)
            for (const auto& vertex : ImGui::GetDrawData()->CmdLists[n]->VtxBuffer) {
                require(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y), "Finite gesture geometry");
                if (vertex.col == IM_COL32(114, 173, 240, 255)) ++outline_vertices;
            }
        require(ImGui::GetCurrentContext()->CurrentWindowStack.empty(), "Balanced chart scopes");
        return enabled ? selection.requested : std::nullopt;
    }
    void point(float fraction) { ImGui::GetIO().AddMousePosEvent(left + fraction * (right - left), (top + bottom) / 2); }
    void press(float fraction) {
        point(fraction); frame(); ImGui::GetIO().AddMouseButtonEvent(0, true);
        require(!frame(), "Mouse-down does not run a query");
    }
    std::optional<FrequencyRange> release(float fraction) {
        point(fraction); frame(); ImGui::GetIO().AddMouseButtonEvent(0, false); return frame();
    }
};

void selection_boundaries() {
    const std::vector<FrequencySummary> odd_width{{905000000, 1001, -80, -40, 60, 6}};
    const auto range = frequency_gesture_range(odd_width, 0, 1, true, 125000);
    require(range && near(range->lower_hz, 904999499.5) && near(range->upper_hz, 905000500.5),
        "Odd-width integer bins preserve their half-Hz edges");
    const auto wide_click = frequency_gesture_range(odd_width, .5, .5, false, 500000);
    require(wide_click && near(wide_click->upper_hz - wide_click->lower_hz, 1001),
        "Oversize click width clamps to available measurements");
    require(!frequency_gesture_range(odd_width, .5, .5, false, -1), "Invalid custom click width cannot apply");
}

void actual_mouse_selection() {
    SelectionUi renderer;
    renderer.press(.255f);
    renderer.point(.505f);
    require(!renderer.frame() && !renderer.frame(), "Dragging must not repeatedly run queries");
    auto forward = renderer.release(.505f);
    require(forward && near(forward->lower_hz, 906745000) && near(forward->upper_hz, 907005000),
        "Drag resolves the actual intersecting frequency bins");
    require(!renderer.frame(), "One gesture emits only one query");
    renderer.press(.505f); auto reverse = renderer.release(.255f);
    require(reverse && near(reverse->lower_hz, forward->lower_hz) && near(reverse->upper_hz, forward->upper_hz),
        "Reverse drag selects the same interval");
    renderer.selection.click_width_hz = 250000;
    renderer.press(.5f); auto click = renderer.release(.5f);
    require(click && near(click->upper_hz - click->lower_hz, 250000) &&
        std::abs((click->lower_hz + click->upper_hz) / 2 - 906995000) < 1000000 / (renderer.right - renderer.left),
        "Click centers the selected channel width at the pointed frequency");
    renderer.press(.02f); auto edge = renderer.release(.02f);
    require(edge && near(edge->lower_hz, 906495000) && near(edge->upper_hz, 906745000),
        "Near-edge click retains width within recorded coverage");
    renderer.press(.25f); auto outside = renderer.release(1.2f);
    require(outside && near(outside->upper_hz, 907495000), "Release outside graph clamps to recorded edge");
    renderer.press(.25f); renderer.point(.75f); renderer.frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); renderer.frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    require(!renderer.release(.75f), "Escape cancels a drag without applying a range");
    ImGui::GetIO().AddMousePosEvent(renderer.left - 20, renderer.top + 20); renderer.frame();
    ImGui::GetIO().AddMouseButtonEvent(0, true); renderer.frame();
    require(!renderer.release(.5f), "Pressing axis labels does not begin frequency selection");
    renderer.selection.highlighted = *forward;
    renderer.frame(); require(renderer.outline_vertices > 0, "Applied interval has a visible blue outline");
    renderer.point(.5f); renderer.frame(false);
    ImGui::GetIO().AddMouseButtonEvent(0, true); renderer.frame(false);
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    require(!renderer.frame(false), "Read-only live chart has no saved-analysis interaction");
    const auto original = renderer.bins;
    renderer.frame();
    for (size_t i = 0; i < original.size(); ++i)
        require(original[i].active_seconds == renderer.bins[i].active_seconds &&
                original[i].observed_seconds == renderer.bins[i].observed_seconds,
                "Selection never modifies measured occupancy");
    renderer.bins.clear(); renderer.press(.5f);
    require(!renderer.release(.5f), "Empty frequency chart cannot produce a selection");
}

std::string contents(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary); return {std::istreambuf_iterator<char>(input), {}};
}
void saved_analysis_link() {
    const auto folder = std::filesystem::current_path() / ("frequency-selection-fixture-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(folder);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{folder};
    const auto file = folder / "synthetic.sqlite";
    ReceiverConfig config; config.synthetic = true; config.center_hz = 1000000000; config.sample_rate = 1000000;
    config.survey_span_hz = 500000; config.tuning_offset_hz = 900; config.lanes.clear();
    const double width = double(config.sample_rate) / 4096;
    {
        SessionStore writer; writer.create(file.string(), config, "synthetic-frequency-selection");
        SpectrumTile tile; tile.id = 1; tile.first_sample = 0; tile.end_sample = 16384; tile.frame_count = 4;
        tile.elapsed_start_seconds = 0; tile.elapsed_end_seconds = .016384;
        tile.utc_start_seconds = 1700000000; tile.utc_end_seconds = 1700000000 + tile.elapsed_end_seconds;
        tile.first_center_hz = double(config.center_hz) - 4 * width; tile.bin_width_hz = width;
        tile.mean_dbfs.assign(9, -60); tile.peak_dbfs.assign(9, -40); tile.background_dbfs = -100;
        tile.activity = {1, 0, 1, 0, 0, 1, 0, 1}; // two frames at each opposite edge
        PositionFix fix; fix.valid = true; fix.latitude = 0; fix.longitude = 0; fix.source = "synthetic";
        tile.receiver_start = fix; tile.receiver_end = fix;
        writer.append(tile);
        Snapshot snapshot; snapshot.config = config; snapshot.elapsed_seconds = .016384;
        snapshot.input_seconds = snapshot.measurement_seconds = .016384; writer.update(snapshot, true);
    }
    const auto before = contents(file);
    Engine engine; std::string error;
    require(engine.open_session(file.string(), error), "Open synthetic saved survey");
    const auto snapshot = engine.snapshot();
    DesktopState ui; ui.config = config; ui.survey_query.time_bucket_seconds = .004096;
    ui.survey_query.geographic_filter = true;
    select_analysis_frequencies(ui, snapshot, {double(config.center_hz) - 4.5 * width, double(config.center_hz) - 3.5 * width});
    require(ui.pending_analysis && ui.pending_analysis->query.geographic_filter &&
        near(ui.pending_analysis->query.time_bucket_seconds, .004096), "Gesture retains time/area filters in queued query");
    require(!ui.analysis_loaded, "Queued query waits until after the updating frame is presented");
    complete_pending_analysis(engine, ui);
    require(ui.analysis_loaded && !ui.pending_analysis && ui.reveal_time_plot && ui.analysis.bins.size() == 1,
        "Gesture updates one shared result for frequency, time and GPS views");
    require(near(ui.analysis.busy_seconds, .008192) && near(ui.analysis.observed_seconds, .016384) &&
        ui.analysis.observations.size() == 4 && ui.analysis.observations.front().receiver_position,
        "Selected-frequency busy time and receiver observations use the saved joint measurements");
    require(analysis_frequency_text(ui.analysis) == "Frequency range: 999.998901 - 999.999146 MHz | Width: 0.244 kHz",
        "Plot caption identifies actual covered frequency edges and bandwidth");
    require(near(ui.analyzed_query.lower_hz, double(config.center_hz) - 4.5 * width) &&
            near(ui.analyzed_query.upper_hz, double(config.center_hz) - 3.5 * width),
        "Applied query remains the result provenance");
    select_analysis_frequencies(ui, snapshot, {}); complete_pending_analysis(engine, ui);
    require(ui.analysis.bins.size() == 9 && near(ui.analysis.busy_seconds, .016384),
        "Full-range reset restores union occupancy from all frequencies");
    const auto applied_lower = ui.analysis.covered_lower_hz;
    ui.query_lower_mhz = 123;
    require(ui.analysis.covered_lower_hz == applied_lower, "Unapplied controls do not relabel existing plots");
    queue_analysis(ui, snapshot, false); ui.pending_analysis->session_id = "different-session";
    complete_pending_analysis(engine, ui);
    require(ui.notice_error && ui.analysis.covered_lower_hz == applied_lower, "Stale queued query cannot apply to a different survey");
    Snapshot unsaved; unsaved.session_id = "memory-only";
    queue_analysis(ui, unsaved, true); require(!ui.pending_analysis && ui.notice_error, "Unsaved survey cannot queue history analysis");
    const auto after = engine.snapshot();
    require(!after.running && after.config.center_hz == config.center_hz && after.config.tuning_offset_hz == 900,
        "Frequency selection never opens or retunes a receiver");
    require(contents(file) == before, "Selection and saved-data queries preserve original database bytes");
}
}

int main() {
    try {
        selection_boundaries(); actual_mouse_selection(); saved_analysis_link();
        std::cout << "Frequency-selection gestures and linked saved analysis passed; no devices opened\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Frequency selection checks failed: " << error.what() << '\n'; return 1;
    }
}
