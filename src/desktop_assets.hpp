// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ovmesh {

struct DesktopFontCandidates {
    std::vector<std::string> ui_sans;
    std::vector<std::string> monospace;
};

// Existing fonts from known local operating-system locations, preferred first.
// This does not load or download fonts. If no candidate loads successfully,
// the caller should retain ImGui's built-in font. Paths use UTF-8.
DesktopFontCandidates system_font_candidates();

inline constexpr uint32_t waveform_png_max_dimension = 8192;
inline constexpr uint64_t waveform_png_max_pixels = 16'777'216;

// Save a plot crop supplied by the caller, never a screen/device capture.
// Pixels are tightly packed 8-bit RGBA. bottom_up accepts OpenGL readback order.
// The PNG contains no metadata; the caller controls which visible plot labels
// are included in its pixels. The new .png file is private and exclusively
// created under the existing local-path policy. No overwrite or folder creation.
// Throws on invalid arguments or a failed write; a failed new file is removed
// only if it is still the file opened by this operation.
void write_waveform_png(const std::string& destination, uint32_t width,
                        uint32_t height, std::span<const uint8_t> rgba,
                        bool bottom_up = false);

} // namespace ovmesh
