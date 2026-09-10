// SPDX-License-Identifier: GPL-3.0-or-later
// Reproduce sparse RF occupancy and narrow peaks in the actual chart. The
// ImGui context remains in memory; no GLFW window, profile or USB is opened.
#include "../src/ui.cpp"
#include <imgui_internal.h>
#include <iostream>
#include <limits>

namespace {
using namespace ovmesh;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

FrequencySummary bin(size_t index, double observed, double active) {
    return {905000000 + index * 2500, 2500, -80, -45, observed, active};
}

void sparse_activity_scale() {
    // Approximately two seconds of activity across an eight-minute survey.
    // On the former 0-100% axis this occupied less than one vertical pixel.
    constexpr double sparse = .00417;
    require(occupancy_height(sparse, OccupancyScale::RevealLowActivity) > .5,
            "Default scale visibly separates sparse measured activity from zero");
    require(occupancy_height(sparse, OccupancyScale::Linear) < .005,
            "Linear scale preserves the small physical occupancy fraction");
    for (const auto scale : {OccupancyScale::RevealLowActivity, OccupancyScale::Linear}) {
        require(occupancy_height(0, scale) == 0 && occupancy_height(1, scale) == 1,
                "Each chart scale retains zero and fully occupied endpoints");
        double previous = -1;
        for (const double value : {0., .0000001, .000001, .0001, sparse, .01, .1, .5, 1.}) {
            const auto height = occupancy_height(value, scale);
            require(std::isfinite(height) && height >= 0 && height <= 1 && height > previous,
                    "Ordered occupancy values remain ordered and finite on either scale");
            previous = height;
        }
    }
}

void pooling_preserves_measurement_meaning() {
    // Both pixels contain unknown and measured bins. Unknown is not quiet;
    // measured zero must not erase the rare burst or a single-bin peak.
    const std::vector<FrequencySummary> bins{
        bin(0, 0, 0), bin(1, 480, 0), bin(2, 480, 2), bin(3, 480, 0),
        bin(4, 480, 0), bin(5, 480, 480), bin(6, 0, 0), bin(7, 480, 0)};
    const auto columns = occupancy_columns(bins, 2);
    require(columns.size() == 2, "Eight bins fit into two bounded chart columns");
    require(columns[0].first == 0 && columns[0].last == 4 &&
            columns[1].first == 4 && columns[1].last == bins.size(),
            "Pooled column ranges account for every source bin exactly once");
    require(columns[0].observed && columns[0].unobserved &&
            columns[1].observed && columns[1].unobserved,
            "A mixed pixel preserves both known measurements and unknown coverage");
    require(std::abs(columns[0].maximum_ratio - 2.0 / 480) < 1e-12 &&
            columns[0].representative == 2,
            "Rare measured activity survives sharing a pixel with three other bins");
    require(columns[1].maximum_ratio == 1 && columns[1].representative == 5,
            "A narrow fully busy peak remains fully visible rather than being averaged away");

    const auto measured_zero = occupancy_columns(std::vector{bin(0, 10, 0)}, 1);
    const auto unknown = occupancy_columns(std::vector{bin(0, 0, 0)}, 1);
    require(measured_zero.size() == 1 && measured_zero[0].observed &&
            !measured_zero[0].unobserved && measured_zero[0].maximum_ratio == 0,
            "Observed zero occupancy remains a measured result");
    require(unknown.size() == 1 && !unknown[0].observed && unknown[0].unobserved,
            "No observation remains unknown rather than a zero-occupancy result");
    require(occupancy_columns(std::vector<FrequencySummary>{}, 100).empty() &&
            occupancy_columns(bins, 0).empty(), "Empty input or no drawable columns is bounded");

    std::vector<FrequencySummary> dense;
    for (size_t i = 0; i < 4096; ++i) dense.push_back(bin(i, 480, 0));
    dense.front().active_seconds = 480;
    dense.back().active_seconds = 480;
    const auto reduced = occupancy_columns(dense, 117);
    require(!reduced.empty() && reduced.size() <= 117 && reduced.front().first == 0 &&
            reduced.back().last == dense.size(), "Dense range fits a small plot without losing either edge");
    require(reduced.front().maximum_ratio == 1 && reduced.back().maximum_ratio == 1,
            "Single-bin peaks at both survey edges survive downsampling");
    size_t next = 0;
    for (const auto& column : reduced) {
        require(column.first == next && column.first < column.last && column.last <= dense.size(),
                "Dense pooled columns neither skip nor overlap source bins");
        next = column.last;
    }
    require(next == dense.size(), "Every dense source bin remains represented");
}

class ChartUi {
public:
    ChartUi() {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.DisplaySize = {380, 280}; io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        require(pixels && width > 0 && height > 0, "Build chart font atlas in memory");
    }
    ~ChartUi() { ImGui::DestroyContext(); }

    float frame(const std::vector<FrequencySummary>& bins, OccupancyScale scale,
                ImVec2 size = {380, 280}) {
        ImGui::GetIO().DisplaySize = size;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(size);
        ImGui::Begin("Occupancy chart fixture", nullptr,
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
        occupancy_chart(bins, 170, scale);
        ImGui::End(); ImGui::Render();
        const auto* context = ImGui::GetCurrentContext();
        require(context->CurrentWindowStack.empty() && context->BeginPopupStack.empty() &&
                context->CurrentTable == nullptr, "Chart rendering balances all ImGui scopes");
        const auto* draw = ImGui::GetDrawData();
        require(draw && draw->Valid && draw->TotalVtxCount > 0,
                "Actual chart generates a nonempty valid draw frame");
        float minimum = std::numeric_limits<float>::infinity();
        float maximum = -std::numeric_limits<float>::infinity();
        for (int list = 0; list < draw->CmdListsCount; ++list) {
            for (const auto& vertex : draw->CmdLists[list]->VtxBuffer) {
                require(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y),
                        "Chart vertices remain finite for empty, sparse and dense observations");
                if (vertex.col == IM_COL32(62, 195, 176, 255)) {
                    minimum = std::min(minimum, vertex.pos.y);
                    maximum = std::max(maximum, vertex.pos.y);
                }
            }
        }
        return std::isfinite(minimum) ? maximum - minimum : 0;
    }
};

void actual_chart_geometry() {
    ChartUi ui;
    for (const auto scale : {OccupancyScale::RevealLowActivity, OccupancyScale::Linear}) {
        ui.frame({}, scale); ui.frame({}, scale);
        require(ui.frame({bin(0, 480, 0)}, scale) == 0,
                "A measured zero draws no activity bar in the actual chart");
        require(ui.frame({bin(0, 0, 0)}, scale) == 0,
                "Unobserved coverage does not use the measured-activity color");
    }
    const auto full = ui.frame({bin(0, 480, 480)}, OccupancyScale::RevealLowActivity);
    const auto sparse = ui.frame({bin(0, 480, 2)}, OccupancyScale::RevealLowActivity);
    const auto linear = ui.frame({bin(0, 480, 2)}, OccupancyScale::Linear);
    require(full > 100 && sparse > full / 2,
            "A real sparse activity bar is plainly visible on the default chart");
    require(linear > 0 && linear < full / 20,
            "Linear chart retains a small but visible positive-activity mark");

    std::vector<FrequencySummary> dense;
    for (size_t i = 0; i < 4096; ++i) dense.push_back(bin(i, i % 3 ? 480 : 0, 0));
    dense[2048].observed_seconds = 480; dense[2048].active_seconds = 480;
    dense[1024].observed_seconds = 480; dense[1024].active_seconds = 2;
    for (const auto scale : {OccupancyScale::RevealLowActivity, OccupancyScale::Linear}) {
        ui.frame(dense, scale, {240, 260});
        const auto height = ui.frame(dense, scale, {240, 260});
        require(std::abs(height - full) <= 1,
                "One full-height frequency peak survives a 4096-bin chart in a narrow window");
    }
    require(dense[2048].active_seconds == 480 && dense[1024].active_seconds == 2 &&
            dense[0].observed_seconds == 0, "Rendering never rewrites retained measurements");
}
} // namespace

int main() {
    try {
        sparse_activity_scale(); pooling_preserves_measurement_meaning(); actual_chart_geometry();
        std::cout << "Occupancy chart sparse-activity and peak-retention checks passed; no windows or USB opened\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Occupancy chart checks failed: " << error.what() << '\n';
        return 1;
    }
}
