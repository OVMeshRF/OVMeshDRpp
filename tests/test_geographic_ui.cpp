// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual receiver-position charts without a window, device,
// preference file or survey database. All positions and RF values are synthetic.
#include "../src/ui.cpp"
#include <imgui_internal.h>
#include <bit>
#include <iostream>
#include <limits>

namespace {
using namespace ovmesh;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

bool same_number(double first, double second) {
    return std::bit_cast<uint64_t>(first) == std::bit_cast<uint64_t>(second);
}
bool same_optional(const std::optional<double>& first, const std::optional<double>& second) {
    return first.has_value() == second.has_value() && (!first || same_number(*first, *second));
}
bool same_fix(const PositionFix& first, const PositionFix& second) {
    return same_number(first.latitude, second.latitude) && same_number(first.longitude, second.longitude) &&
        same_optional(first.altitude_m, second.altitude_m) && same_number(first.utc_seconds, second.utc_seconds) &&
        same_number(first.monotonic_seconds, second.monotonic_seconds) && first.valid == second.valid &&
        first.manual == second.manual && first.source == second.source && same_optional(first.hdop, second.hdop) &&
        first.satellites == second.satellites;
}
bool same_observation(const SurveyObservation& first, const SurveyObservation& second) {
    return same_number(first.elapsed_start, second.elapsed_start) && same_number(first.elapsed_end, second.elapsed_end) &&
        same_number(first.observed_seconds, second.observed_seconds) && same_number(first.busy_seconds, second.busy_seconds) &&
        same_number(first.outside_center_busy_seconds, second.outside_center_busy_seconds) &&
        same_number(first.center_busy_seconds, second.center_busy_seconds) &&
        same_number(first.mean_dbfs, second.mean_dbfs) && same_number(first.peak_dbfs, second.peak_dbfs) &&
        same_number(first.background_dbfs, second.background_dbfs) && first.quality == second.quality &&
        first.receiver_position.has_value() == second.receiver_position.has_value() &&
        (!first.receiver_position || same_fix(*first.receiver_position, *second.receiver_position));
}

PositionFix position(double latitude, double longitude, double seconds) {
    PositionFix fix;
    fix.latitude = latitude; fix.longitude = longitude; fix.utc_seconds = 1700000000 + seconds;
    fix.monotonic_seconds = seconds; fix.altitude_m = 125.5; fix.valid = true;
    fix.hdop = 1.25; fix.satellites = 9; fix.source = "synthetic-test";
    return fix;
}
SurveyObservation observation(const std::optional<PositionFix>& fix, double seconds, double busy = .25) {
    SurveyObservation result;
    result.elapsed_start = seconds; result.elapsed_end = seconds + 1;
    result.observed_seconds = 1; result.busy_seconds = busy;
    result.outside_center_busy_seconds = busy;
    result.mean_dbfs = -72.25; result.peak_dbfs = -42.5; result.background_dbfs = -85.75;
    result.quality = SurveyUncalibrated; result.receiver_position = fix;
    return result;
}

enum class Chart { Track, Rf, BusyTime };
struct DrawSummary {
    size_t route_vertices = 0, reference_vertices = 0;
    std::vector<ImDrawVert> vertices;
    std::vector<std::pair<float, float>> busy_triangle_x_ranges;
    size_t color_count(ImU32 color) const {
        return static_cast<size_t>(std::count_if(vertices.begin(), vertices.end(),
            [color](const auto& vertex) { return vertex.col == color; }));
    }
    std::optional<ImVec4> bounds(ImU32 color) const {
        std::optional<ImVec4> result;
        for (const auto& vertex : vertices) if (vertex.col == color) {
            if (!result) result = ImVec4{vertex.pos.x, vertex.pos.y, vertex.pos.x, vertex.pos.y};
            result->x = std::min(result->x, vertex.pos.x); result->y = std::min(result->y, vertex.pos.y);
            result->z = std::max(result->z, vertex.pos.x); result->w = std::max(result->w, vertex.pos.y);
        }
        return result;
    }
};

class GeographicUi {
public:
    GeographicUi() {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.LogFilename = nullptr; io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr; int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        require(pixels && width > 0 && height > 0, "Build geographic fixture font atlas in memory");
    }
    ~GeographicUi() { ImGui::DestroyContext(); }

    DrawSummary frame(Chart chart, Snapshot& snapshot, DesktopState& ui, ImVec2 size = {1100, 650}) {
        auto& io = ImGui::GetIO(); io.DisplaySize = size; io.AddMousePosEvent(-10000, -10000);
        ImGui::NewFrame(); ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(size);
        ImGui::Begin("Geographic chart fixture", nullptr,
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
        if (chart == Chart::Track) track_view(snapshot, ui, 220);
        else if (chart == Chart::Rf) geographic_rf_view(ui, 240);
        else busy_time_chart(ui, 220);
        ImGui::End(); ImGui::Render();
        const auto* context = ImGui::GetCurrentContext();
        require(context->CurrentWindowStack.empty() && context->BeginPopupStack.empty() &&
                context->CurrentTable == nullptr, "Geographic rendering balances all ImGui scopes");
        const auto* draw = ImGui::GetDrawData();
        require(draw && draw->Valid && draw->TotalVtxCount > 0, "Geographic chart produces a valid draw frame");
        const auto route_color = chart == Chart::Track ? IM_COL32(57, 169, 170, 210) : IM_COL32(84, 110, 134, 140);
        DrawSummary result;
        for (int list = 0; list < draw->CmdListsCount; ++list) {
            for (const auto& vertex : draw->CmdLists[list]->VtxBuffer) {
                require(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y),
                        "Geographic geometry remains finite, including missing and invalid fixes");
                if (vertex.col == route_color) ++result.route_vertices;
                if (vertex.col == IM_COL32(239, 250, 243, 255)) ++result.reference_vertices;
                result.vertices.push_back(vertex);
            }
            if (chart == Chart::BusyTime) {
                const auto* list_data = draw->CmdLists[list];
                for (const auto& command : list_data->CmdBuffer) {
                    if (command.UserCallback) continue;
                    for (unsigned index = 0; index + 2 < command.ElemCount; index += 3) {
                        const auto& a = list_data->VtxBuffer[command.VtxOffset + list_data->IdxBuffer[command.IdxOffset + index]];
                        const auto& b = list_data->VtxBuffer[command.VtxOffset + list_data->IdxBuffer[command.IdxOffset + index + 1]];
                        const auto& c = list_data->VtxBuffer[command.VtxOffset + list_data->IdxBuffer[command.IdxOffset + index + 2]];
                        constexpr ImU32 busy_color = IM_COL32(62, 195, 176, 255);
                        if (a.col == busy_color && b.col == busy_color && c.col == busy_color)
                            result.busy_triangle_x_ranges.emplace_back(std::min({a.pos.x, b.pos.x, c.pos.x}),
                                                                      std::max({a.pos.x, b.pos.x, c.pos.x}));
                    }
                }
            }
        }
        return result;
    }
};

void set_positions(Snapshot& snapshot, DesktopState& ui, const std::vector<PositionFix>& positions) {
    snapshot.track = positions; ui.analysis.observations.clear();
    for (const auto& fix : positions)
        ui.analysis.observations.push_back(observation(fix, fix.monotonic_seconds));
    ui.analyzed_query.time_bucket_seconds = 1;
    ui.analysis.effective_time_bucket_seconds = 1;
    ui.analysis.outside_center_bin_count = 1;
    ui.analysis.resolved_elapsed_start = positions.empty() ? 0 : positions.front().monotonic_seconds;
    ui.analysis.resolved_elapsed_end = positions.empty() ? 0 : positions.back().monotonic_seconds + 1;
}

void stationary_is_not_a_route(GeographicUi& renderer) {
    Snapshot snapshot; DesktopState ui;
    require(ui.position_view_mode == PositionViewMode::Stationary,
            "Position displays default to stationary rather than implying a receiver route");
    // Several metres of synthetic fix scatter around one stationary receiver.
    set_positions(snapshot, ui, {
        position(45.00001, 12.00001, 1), position(44.99999, 11.99998, 2),
        position(45.00002, 11.99999, 3), position(44.99998, 12.00002, 4)});
    ui.analysis.observations[1].busy_seconds = .75;
    ui.analysis.observations[1].outside_center_busy_seconds = .5;
    ui.analysis.observations[1].center_busy_seconds = .25;
    ui.analysis.observations[2].quality |= SurveyClipped;
    const auto original_track = snapshot.track;
    const auto original_observations = ui.analysis.observations;
    for (const auto chart : {Chart::Track, Chart::Rf}) {
        renderer.frame(chart, snapshot, ui);
        const auto stationary = renderer.frame(chart, snapshot, ui);
        require(stationary.route_vertices == 0 && stationary.reference_vertices > 0,
                "Stationary scatter has a neutral reference and no route-colored connecting lines");
        ui.position_view_mode = PositionViewMode::Mobile;
        require(renderer.frame(chart, snapshot, ui).route_vertices > 0,
                "Explicit mobile view connects successive nearby-in-time valid fixes");
        ui.position_view_mode = PositionViewMode::Stationary;
        require(renderer.frame(chart, snapshot, ui).route_vertices == 0,
                "Returning to stationary view removes the route without requiring new measurements");
    }
    require(snapshot.track.size() == original_track.size() &&
            ui.analysis.observations.size() == original_observations.size(), "Changing view preserves sample counts");
    for (size_t i = 0; i < original_track.size(); ++i)
        require(same_fix(snapshot.track[i], original_track[i]) &&
                same_observation(ui.analysis.observations[i], original_observations[i]),
                "View changes preserve every raw fix field and RF observation value exactly");
}

void gaps_never_become_routes(GeographicUi& renderer) {
    Snapshot snapshot; DesktopState ui; ui.position_view_mode = PositionViewMode::Mobile;
    const auto first = position(45, 12, 1), last = position(45.00002, 12.00002, 3);
    std::vector<PositionFix> gaps;
    auto invalid = position(45.00001, 12.00001, 2); invalid.valid = false; gaps.push_back(invalid);
    invalid.valid = true; invalid.latitude = std::numeric_limits<double>::quiet_NaN(); gaps.push_back(invalid);
    invalid.latitude = 45; invalid.longitude = std::numeric_limits<double>::infinity(); gaps.push_back(invalid);
    invalid.longitude = 12; invalid.latitude = 91; gaps.push_back(invalid);
    invalid.latitude = 45; invalid.longitude = 181; gaps.push_back(invalid);
    for (const auto& gap : gaps) {
        set_positions(snapshot, ui, {first, gap, last});
        for (const auto chart : {Chart::Track, Chart::Rf})
            require(renderer.frame(chart, snapshot, ui).route_vertices == 0,
                    "Invalid or nonfinite receiver fix breaks a mobile route instead of being skipped across");
    }
    set_positions(snapshot, ui, {first, last});
    ui.analysis.observations.insert(ui.analysis.observations.begin() + 1, observation(std::nullopt, 2));
    require(renderer.frame(Chart::Rf, snapshot, ui).route_vertices == 0,
            "Missing receiver position breaks the geographic RF route");
    set_positions(snapshot, ui, {first, position(45.00002, 12.00002, 100)});
    for (const auto chart : {Chart::Track, Chart::Rf})
        require(renderer.frame(chart, snapshot, ui).route_vertices == 0,
                "A long unobserved time interval is not drawn as a continuous receiver route");
}

void edge_cases_render(GeographicUi& renderer) {
    Snapshot snapshot; DesktopState ui;
    auto invalid = position(45, 12, 1); invalid.valid = false;
    std::vector<std::vector<PositionFix>> fixtures{{}, {position(45, 12, 1)}, {invalid}};
    std::vector<PositionFix> repeated;
    for (size_t i = 0; i < 256; ++i) repeated.push_back(position(45, 12, 1 + static_cast<double>(i)));
    fixtures.push_back(repeated);
    // Finite coordinates near projection boundaries remain bounded on screen.
    fixtures.push_back({position(89.999, 179.999, 1), position(89.999, -179.999, 2)});
    for (const auto& positions : fixtures) {
        set_positions(snapshot, ui, positions);
        for (const auto mode : {PositionViewMode::Stationary, PositionViewMode::Mobile}) {
            ui.position_view_mode = mode;
            for (const auto chart : {Chart::Track, Chart::Rf}) {
                for (const auto size : {ImVec2{300, 440}, ImVec2{1300, 650}}) {
                    renderer.frame(chart, snapshot, ui, size);
                    const auto result = renderer.frame(chart, snapshot, ui, size);
                    if (mode == PositionViewMode::Stationary || positions.size() < 2)
                        require(result.route_vertices == 0, "Empty, single and stationary displays cannot imply a route");
                }
            }
        }
    }
}

void guard_selection_changes_display_only(GeographicUi& renderer) {
    Snapshot snapshot; DesktopState ui;
    require(ui.use_center_guard, "Analysis defaults to a labeled receiver-center guard view");
    set_positions(snapshot, ui, {position(45, 12, 1)});
    ui.analysis.center_guard_bin_count = 5; ui.analysis.outside_center_bin_count = 100;
    auto& measured = ui.analysis.observations.front();
    measured.busy_seconds = 1; measured.center_busy_seconds = 1; measured.outside_center_busy_seconds = 0;
    const auto original = measured;
    const auto zero_color = ImGui::ColorConvertFloat4ToU32({.22f, .72f, .82f, 1});
    const auto full_color = ImGui::ColorConvertFloat4ToU32({.98f, .37f, .20f, 1});
    const auto quarter_color = ImGui::ColorConvertFloat4ToU32({.41f, .6325f, .665f, 1});
    require(guarded_view(ui) && displayed_busy_ratio(ui, measured) == std::optional<double>{0},
            "Continuous center-only activity displays measured zero outside the guard");
    renderer.frame(Chart::Rf, snapshot, ui);
    auto shown = renderer.frame(Chart::Rf, snapshot, ui);
    require(shown.color_count(zero_color) > 0 && shown.color_count(full_color) == 0,
            "Geographic color uses the outside-center zero rather than raw center-only saturation");
    const auto measured_zero = renderer.frame(Chart::BusyTime, snapshot, ui);
    require(measured_zero.color_count(IM_COL32(62, 195, 176, 255)) == 0 &&
            measured_zero.color_count(IM_COL32(90, 115, 133, 255)) > 0,
            "True measured zero shows observation coverage without an active time-chart fill");
    ui.use_center_guard = false;
    require(!guarded_view(ui) && displayed_busy_ratio(ui, measured) == std::optional<double>{1},
            "Raw comparison restores the original 100-percent busy fraction");
    require(renderer.frame(Chart::Rf, snapshot, ui).color_count(full_color) > 0,
            "Geographic raw comparison visibly restores the original busy color");
    require(renderer.frame(Chart::BusyTime, snapshot, ui).color_count(IM_COL32(62, 195, 176, 255)) > 0,
            "Raw comparison restores the original time-chart activity");
    ui.use_center_guard = true;
    require(same_observation(measured, original), "Changing the center-guard view preserves every observation field");

    measured.outside_center_busy_seconds = .25;
    require(displayed_busy_ratio(ui, measured) == std::optional<double>{.25},
            "Guarded display uses the measured 25-percent outside-center union");
    require(renderer.frame(Chart::Rf, snapshot, ui).color_count(quarter_color) > 0,
            "Geographic palette agrees with the guarded busy fraction");
    ui.analysis.outside_center_bin_count = 0;
    require(!displayed_busy_ratio(ui, measured), "An all-guard frequency selection is unavailable, not quiet");
    shown = renderer.frame(Chart::Rf, snapshot, ui);
    require(shown.color_count(zero_color) == 0 && shown.color_count(quarter_color) == 0 &&
            shown.color_count(full_color) == 0, "All-guard geographic observations do not use a measured busy color");
    const auto unavailable = renderer.frame(Chart::BusyTime, snapshot, ui);
    require(unavailable.color_count(IM_COL32(62, 195, 176, 255)) == 0 &&
            unavailable.color_count(IM_COL32(90, 115, 133, 255)) == 0,
            "All-guard unavailable observations do not masquerade as measured zero exposure");
    ui.use_center_guard = false;
    require(displayed_busy_ratio(ui, measured) == std::optional<double>{1},
            "Raw data remain available for an all-guard comparison");
    measured.observed_seconds = 0;
    for (const bool enabled : {false, true}) {
        ui.use_center_guard = enabled;
        require(!displayed_busy_ratio(ui, measured), "No observed time remains unavailable in either display mode");
    }
    measured = original; ui.analysis.center_guard_bin_count = 0; ui.analysis.outside_center_bin_count = 1;
    require(!guarded_view(ui) && displayed_busy_ratio(ui, measured) == std::optional<double>{1},
            "A range with no center-guard bins retains its raw occupancy");
}

void complete_elapsed_axis_and_gaps(GeographicUi& renderer) {
    Snapshot snapshot; DesktopState ui;
    ui.analysis.resolved_elapsed_start = 0; ui.analysis.resolved_elapsed_end = 100;
    ui.analysis.effective_time_bucket_seconds = 1; ui.analysis.outside_center_bin_count = 1;
    ui.analysis.observations = {observation(std::nullopt, 90, .5)};
    renderer.frame(Chart::BusyTime, snapshot, ui);
    auto shown = renderer.frame(Chart::BusyTime, snapshot, ui);
    const auto background = shown.bounds(IM_COL32(7, 14, 22, 255));
    const auto activity = shown.bounds(IM_COL32(62, 195, 176, 255));
    require(background && activity && activity->x > background->x + (background->z - background->x) * .8f &&
            activity->z <= background->z && activity->y >= background->y && activity->w <= background->w,
            "Late measurements remain near the end of the full selected elapsed span and inside the chart");
    ui.analysis.observations = {observation(std::nullopt, 0, .5), observation(std::nullopt, 99, .5)};
    shown = renderer.frame(Chart::BusyTime, snapshot, ui);
    const auto area = shown.bounds(IM_COL32(7, 14, 22, 255));
    require(area && !shown.busy_triangle_x_ranges.empty(), "Both endpoint observations produce time-chart activity");
    const float middle = (area->x + area->z) / 2;
    for (const auto& interval : shown.busy_triangle_x_ranges)
        require(!(interval.first < middle && interval.second > middle),
                "An unobserved middle interval remains blank rather than stretching a measured bucket across it");
    CoverageGap gap; gap.elapsed_start_seconds = 40; gap.elapsed_end_seconds = 60; gap.reason = "synthetic-gap";
    ui.analysis.gaps.push_back(gap);
    const auto with_gap = renderer.frame(Chart::BusyTime, snapshot, ui);
    require(with_gap.color_count(IM_COL32(100, 105, 115, 130)) > 0,
            "Recorded coverage gaps have a distinct striped display rather than the measured-zero appearance");
    for (const auto& interval : with_gap.busy_triangle_x_ranges)
        require(!(interval.first < middle && interval.second > middle),
                "A recorded coverage gap does not acquire busy fill");
    for (const auto size : {ImVec2{300, 440}, ImVec2{1300, 650}}) {
        renderer.frame(Chart::BusyTime, snapshot, ui, size);
        ui.analysis.observations.clear(); renderer.frame(Chart::BusyTime, snapshot, ui, size);
    }

    set_positions(snapshot, ui, {position(45, 12, 1), position(45.00002, 12.00002, 3)});
    ui.position_view_mode = PositionViewMode::Mobile;
    ui.analyzed_query.time_bucket_seconds = 100;
    ui.analysis.effective_time_bucket_seconds = .1;
    require(renderer.frame(Chart::Rf, snapshot, ui).route_vertices == 0,
            "Requested bucket width cannot bridge a gap exceeding the effective display bucket width");
    ui.analyzed_query.time_bucket_seconds = .1;
    ui.analysis.effective_time_bucket_seconds = 10;
    require(renderer.frame(Chart::Rf, snapshot, ui).route_vertices > 0,
            "Geographic connectivity uses actual coarsened display buckets rather than the requested width");
}
} // namespace

int main() {
    try {
        GeographicUi renderer;
        stationary_is_not_a_route(renderer); gaps_never_become_routes(renderer); edge_cases_render(renderer);
        guard_selection_changes_display_only(renderer); complete_elapsed_axis_and_gaps(renderer);
        std::cout << "Geographic/analysis UI stationary/mobile, center-guard, elapsed-coverage and data-preservation checks passed; no windows or USB opened\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Geographic UI checks failed: " << error.what() << '\n';
        return 1;
    }
}
