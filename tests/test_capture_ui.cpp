// SPDX-License-Identifier: GPL-3.0-or-later
// Pure crop and ImGui overlay checks. No window, GL context, radio, GPS or data.
#include "../src/ui.cpp"
#include <iostream>

namespace {
using namespace ovmesh;
size_t checks = 0;
void require(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F operation, const char* message) {
    ++checks;
    try { operation(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

void crop_coordinates() {
    ImDrawData draw;
    draw.Valid = true; draw.DisplayPos = {0, 0}; draw.DisplaySize = {1000, 600};
    auto crop = waveform_framebuffer_crop({100, 50}, {400, 200}, draw, 1000, 600);
    require(crop.x == 100 && crop.y == 350 && crop.width == 400 && crop.height == 200, "Logical crop becomes GL bottom-left coordinates");
    crop = waveform_framebuffer_crop({100, 50}, {400, 200}, draw, 2000, 1200);
    require(crop.x == 200 && crop.y == 700 && crop.width == 800 && crop.height == 400, "Retina framebuffer scaling");
    draw.DisplayPos = {200, 300};
    crop = waveform_framebuffer_crop({300, 350}, {400, 200}, draw, 2000, 1200);
    require(crop.x == 200 && crop.y == 700 && crop.width == 800 && crop.height == 400, "Viewport display offset is removed before scaling");
    draw.DisplayPos = {0, 0};
    crop = waveform_framebuffer_crop({100.25f, 50.25f}, {400.5f, 200.5f}, draw, 1500, 900);
    require(crop.x == 151 && crop.y == 524 && crop.width == 600 && crop.height == 300, "Fractional DPI rounds inward to exclude neighboring controls");
    crop = waveform_framebuffer_crop({0, 0}, {1000, 600}, draw, 2000, 1200);
    require(crop.x == 0 && crop.y == 0 && crop.width == 2000 && crop.height == 1200, "Entire visible crop remains bounded");
    for (const auto& pair : {std::pair{ImVec2{-1, 0}, ImVec2{20, 20}},
                             {ImVec2{0, -1}, ImVec2{20, 20}}, {ImVec2{990, 0}, ImVec2{20, 20}},
                             {ImVec2{0, 590}, ImVec2{20, 20}}, {ImVec2{0, 0}, ImVec2{0, 20}},
                             {ImVec2{0, 0}, ImVec2{20, -1}}})
        rejects([&] { waveform_framebuffer_crop(pair.first, pair.second, draw, 1000, 600); }, "Offscreen or empty plot cannot include adjacent screen areas");
    rejects([&] { waveform_framebuffer_crop({0, 0}, {1000, 600}, draw, 16000, 9600); }, "PNG dimensions remain bounded after DPI scaling");
    rejects([&] { waveform_framebuffer_crop({0, 0}, {1000, 600}, draw, 8000, 4800); }, "Pixel-count limit applies independently of edge limit");
    rejects([&] { waveform_framebuffer_crop({0, 0}, {100, 100}, draw, 0, 600); }, "Minimized/zero framebuffer rejected");
    rejects([&] { waveform_framebuffer_crop({0, 0}, {.1f, .1f}, draw, 1000, 600); }, "Subpixel empty crop rejected");
    const float nan = std::numeric_limits<float>::quiet_NaN(), infinity = std::numeric_limits<float>::infinity();
    rejects([&] { waveform_framebuffer_crop({nan, 0}, {20, 20}, draw, 1000, 600); }, "Nonfinite origin rejected");
    rejects([&] { waveform_framebuffer_crop({0, 0}, {infinity, 20}, draw, 1000, 600); }, "Nonfinite extent rejected");
    draw.Valid = false;
    rejects([&] { waveform_framebuffer_crop({0, 0}, {20, 20}, draw, 1000, 600); }, "Unrendered frame rejected");
    draw.Valid = true; draw.DisplaySize.x = nan;
    rejects([&] { waveform_framebuffer_crop({0, 0}, {20, 20}, draw, 1000, 600); }, "Nonfinite display scale rejected");
}

void overlays() {
    ImGui::CreateContext();
    DesktopState ui;
    require(!waveform_capture_has_overlay(ui), "Unobstructed plot allowed");
    for (auto* flag : {&ui.show_settings, &ui.show_keys, &ui.show_detail, &ui.show_export,
                       &ui.show_licenses, &ui.show_diagnostics, &ui.show_analysis_details,
                       &ui.new_requested, &ui.close_requested}) {
        *flag = true;
        require(waveform_capture_has_overlay(ui), "Each app overlay blocks crop capture");
        *flag = false;
    }
    ui.file_picker.purpose = FilePickerPurpose::SaveCopy;
    require(waveform_capture_has_overlay(ui), "A file chooser blocks crop capture");
    ui.file_picker.purpose = FilePickerPurpose::None;
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.DisplaySize = {1000, 600}; io.DeltaTime = 1.f / 60;
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGui::NewFrame(); ImGui::Begin("Synthetic capture fixture");
    ImGui::OpenPopup("Any popup"); ImGui::End(); ImGui::Render();
    require(waveform_capture_has_overlay(ui), "Untracked generic ImGui popup also blocks capture after Render");
    ImGui::DestroyContext();
}
} // namespace

int main() {
    try {
        crop_coordinates(); overlays();
        std::cout << "Capture UI: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Capture UI failed: " << error.what() << '\n';
        return 1;
    }
}
