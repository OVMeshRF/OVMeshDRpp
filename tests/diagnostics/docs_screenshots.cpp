// SPDX-License-Identifier: GPL-3.0-or-later
// Render the actual desktop UI against an explicitly supplied synthetic survey.
// No receiver start, device inventory, GPS connection, preferences or UI input.
// The caller must separately review the input and resulting pixels for privacy;
// a synthetic flag alone is not proof that a database contains no private data.
#include "../../src/ui.cpp"
#include <iostream>

int main(int argc, char** argv) {
    using namespace ovmesh;
    namespace fs = std::filesystem;
    if (argc != 3) {
        std::cerr << "Usage: docs-screenshots SYNTHETIC_SURVEY NEW_OUTPUT_DIRECTORY\n";
        return 2;
    }
    try {
        Engine engine;
        std::string error;
        if (!engine.open_session(argv[1], error)) throw std::runtime_error(error);
        const auto snapshot = engine.snapshot();
        if (!snapshot.historical || !snapshot.config.synthetic || snapshot.running)
            throw std::runtime_error("Only a stopped synthetic survey is accepted");
        const std::string output_argument = argv[2];
        const auto output = fs::absolute(fs::path(std::u8string(output_argument.begin(), output_argument.end())));
        if (!fs::create_directory(output)) throw std::runtime_error("Output directory must be new");
        fs::permissions(output, fs::perms::owner_all);
        DesktopState ui;
        ui.passive_smoke = true;
        ui.config = snapshot.config;
        ui.config.session_path.clear();
        ui.source = 0;
        ui.spectrum_only = !snapshot.config.discover_lora;
        ui.save_session = false;
        ui.focus_analysis = true;
        ui.query_lower_mhz = 906.75;
        ui.query_upper_mhz = 907;
        ui.width_choice = 2;
        ui.query_width_khz = 250;
        ui.survey_query.lower_hz = 906750000;
        ui.survey_query.upper_hz = 907000000;
        if (!engine.analyze_survey(ui.survey_query, ui.analysis, error)) throw std::runtime_error(error);
        ui.analysis_loaded = true;
        ui.analysis_session = snapshot.session_id;
        ui.analyzed_query = ui.survey_query;
        ui.last_analysis_refresh = 0;
        ui.notice = "Synthetic documentation example / no radio or GPS access";
        ReportOptions report;
        report.kind = ReportKind::Analysis;
        report.query = ui.survey_query;
        if (!engine.export_report(path_utf8(output / "survey-analysis.html"), report, error))
            throw std::runtime_error(error);
        if (!glfwInit()) throw std::runtime_error("Desktop window initialization failed");
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
#if defined(__APPLE__)
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_FALSE);
#endif
        GLFWwindow* window = glfwCreateWindow(1460, 940, "Synthetic documentation renderer", nullptr, nullptr);
        if (!window) throw std::runtime_error("OpenGL window creation failed");
        glfwMakeContextCurrent(window);
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        setup_style();
        const auto fonts = system_font_candidates();
        const auto load_font = [&](const std::vector<std::string>& candidates) -> ImFont* {
            for (const auto& path : candidates)
                if (auto* font = io.Fonts->AddFontFromFileTTF(path.c_str(), 14.f)) return font;
            return io.Fonts->AddFontDefault();
        };
        ui.sans_font = load_font(fonts.ui_sans);
        ui.mono_font = load_font(fonts.monospace);
        io.FontDefault = ui.sans_font;
        if (!ImGui_ImplGlfw_InitForOpenGL(window, false) || !ImGui_ImplOpenGL3_Init("#version 150"))
            throw std::runtime_error("Desktop renderer initialization failed");
        const auto capture = [&](const char* name) {
            for (int frame = 0; frame < 4; ++frame) {
                glfwPollEvents();
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();
                render(engine, ui, snapshot);
                ImGui::Render();
                int width = 0, height = 0;
                glfwGetFramebufferSize(window, &width, &height);
                glViewport(0, 0, width, height);
                glClearColor(.045f, .058f, .075f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                if (frame == 3) {
                    if (width <= 0 || height <= 0 || uint64_t(width) * uint64_t(height) > waveform_png_max_pixels)
                        throw std::runtime_error("Unexpected framebuffer size");
                    std::vector<uint8_t> rgba(size_t(width) * size_t(height) * 4);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                    if (glGetError() != GL_NO_ERROR) throw std::runtime_error("Framebuffer readback failed");
                    write_waveform_png(path_utf8(output / name), uint32_t(width), uint32_t(height), rgba, true);
                }
                glfwSwapBuffers(window);
            }
        };
        capture("analyze-frequency.png");
        ui.analysis_view = 1;
        capture("analyze-time.png");
        ui.show_settings = true;
        ui.settings_page = 3;
        capture("settings-display.png");
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        std::cout << "Rendered three synthetic documentation views; no hardware access.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
