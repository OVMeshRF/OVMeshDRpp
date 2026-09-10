// SPDX-License-Identifier: GPL-3.0-or-later
// Included within ui.cpp's private namespace, after ui_workflow.hpp. The caller
// invokes capture_waveform_frame after rendering and before swapping GL_BACK.

struct WaveformFramebufferCrop {
    int x = 0, y = 0, width = 0, height = 0;
};

WaveformFramebufferCrop waveform_framebuffer_crop(ImVec2 origin, ImVec2 size,
                                                   const ImDrawData& draw,
                                                   int framebuffer_width, int framebuffer_height) {
    const double x = static_cast<double>(origin.x) - draw.DisplayPos.x;
    const double y = static_cast<double>(origin.y) - draw.DisplayPos.y;
    const double right = x + size.x, bottom = y + size.y;
    if (!draw.Valid || framebuffer_width <= 0 || framebuffer_height <= 0 ||
        !std::isfinite(draw.DisplaySize.x) || !std::isfinite(draw.DisplaySize.y) ||
        draw.DisplaySize.x <= 0 || draw.DisplaySize.y <= 0 ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(right) || !std::isfinite(bottom) ||
        size.x <= 0 || size.y <= 0 || x < 0 || y < 0 ||
        right > draw.DisplaySize.x || bottom > draw.DisplaySize.y)
        throw std::runtime_error("Show the complete spectrum and waterfall before capturing a PNG.");
    const double scale_x = framebuffer_width / static_cast<double>(draw.DisplaySize.x);
    const double scale_y = framebuffer_height / static_cast<double>(draw.DisplaySize.y);
    // Round inward: never include an adjacent UI pixel outside the plot crop.
    const int left_pixel = static_cast<int>(std::ceil(x * scale_x));
    const int top_pixel = static_cast<int>(std::ceil(y * scale_y));
    const int right_pixel = static_cast<int>(std::floor(right * scale_x));
    const int bottom_pixel = static_cast<int>(std::floor(bottom * scale_y));
    const int width = right_pixel - left_pixel, height = bottom_pixel - top_pixel;
    if (width <= 0 || height <= 0 || width > static_cast<int>(waveform_png_max_dimension) ||
        height > static_cast<int>(waveform_png_max_dimension) ||
        static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > waveform_png_max_pixels)
        throw std::runtime_error("Resize the spectrum view to a supported PNG size, then capture again.");
    return {left_pixel, framebuffer_height - bottom_pixel, width, height};
}

bool waveform_capture_has_overlay(const DesktopState& ui) {
    return ui.show_settings || ui.show_keys || ui.show_detail || ui.show_export || ui.show_licenses ||
        ui.show_diagnostics || ui.show_analysis_details || ui.new_requested || ui.close_requested ||
        ui.file_picker.purpose != FilePickerPurpose::None ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

class WaveformPixelPackState {
public:
    WaveformPixelPackState() {
        for (size_t index = 0; index < fields_.size(); ++index)
            glGetIntegerv(fields_[index], &values_[index]);
        glGetIntegerv(GL_READ_BUFFER, &read_buffer_);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
        glPixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
        glReadBuffer(GL_BACK);
    }
    ~WaveformPixelPackState() {
        for (size_t index = 0; index < fields_.size(); ++index)
            glPixelStorei(fields_[index], values_[index]);
        glReadBuffer(static_cast<GLenum>(read_buffer_));
    }
    WaveformPixelPackState(const WaveformPixelPackState&) = delete;
    WaveformPixelPackState& operator=(const WaveformPixelPackState&) = delete;
private:
    const std::array<GLenum, 6> fields_{GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS,
                                      GL_PACK_SKIP_ROWS, GL_PACK_SWAP_BYTES, GL_PACK_LSB_FIRST};
    std::array<GLint, 6> values_{};
    GLint read_buffer_ = GL_BACK;
};

void capture_waveform_frame(GLFWwindow* window, Engine& engine, DesktopState& ui, const Snapshot& snapshot) {
    (void)engine; // Capture never reads or changes receiver/GPS state.
    if (!ui.capture_requested) return;
    ui.capture_requested = false;
    ui.capture_pixels.clear();
    try {
        if (!window || ui.operation_busy() || snapshot.spectrum_dbfs.empty() || waveform_capture_has_overlay(ui))
            throw std::runtime_error("Close dialogs and show the waveform before capturing a PNG.");
        const auto* draw = ImGui::GetDrawData();
        if (!draw) throw std::runtime_error("The waveform has not rendered yet.");
        int framebuffer_width = 0, framebuffer_height = 0;
        glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
        const auto crop = waveform_framebuffer_crop(ui.capture_origin, ui.capture_size, *draw,
                                                     framebuffer_width, framebuffer_height);
        if (glGetError() != GL_NO_ERROR)
            throw std::runtime_error("OpenGL reported an error before capture. No PNG was created.");
        // OpenGL 3.2 is the desktop's existing minimum. Keep these enum values
        // local so Windows' legacy GL headers need no new function loader.
        constexpr GLenum pixel_pack_buffer_binding = 0x88ed;
        constexpr GLenum read_framebuffer_binding = 0x8caa;
        GLint pack_buffer = 0, read_framebuffer = 0;
        glGetIntegerv(pixel_pack_buffer_binding, &pack_buffer);
        glGetIntegerv(read_framebuffer_binding, &read_framebuffer);
        if (glGetError() != GL_NO_ERROR || pack_buffer != 0 || read_framebuffer != 0)
            throw std::runtime_error("PNG capture requires the window back buffer and no pixel-pack buffer.");
        std::vector<uint8_t> pixels(static_cast<size_t>(crop.width) * static_cast<size_t>(crop.height) * 4);
        {
            WaveformPixelPackState restore;
            glReadPixels(crop.x, crop.y, crop.width, crop.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            if (glGetError() != GL_NO_ERROR) throw std::runtime_error("The waveform image could not be read. No PNG was created.");
        }
        ui.capture_width = static_cast<uint32_t>(crop.width);
        ui.capture_height = static_cast<uint32_t>(crop.height);
        ui.capture_pixels = std::move(pixels);
        begin_file_picker(ui, FilePickerPurpose::WaveformImage, ui.capture_path, snapshot.config.session_path);
        ui.file_chosen = [&ui](const std::string& path) {
            if (ui.operation_busy()) throw std::runtime_error("Wait for the current operation before saving the PNG.");
            const uint32_t width = ui.capture_width, height = ui.capture_height;
            auto captured = std::move(ui.capture_pixels);
            ui.capture_path = path;
            ui.begin_operation("Saving waveform PNG...", [path, width, height, pixels = std::move(captured)] {
                write_waveform_png(path, width, height, pixels, true);
                return DesktopState::OperationResult{true, "Waveform PNG saved. Reception is unchanged."};
            });
        };
    } catch (const std::exception& error) {
        ui.capture_pixels.clear();
        ui.feedback(false, error.what());
    }
}
