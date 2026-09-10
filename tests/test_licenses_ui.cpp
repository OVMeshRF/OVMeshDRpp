// SPDX-License-Identifier: GPL-3.0-or-later
// Embedded notice fidelity and real ImGui dialog rendering; no OS/device access.
#include "../src/ui.cpp"
#include <imgui_internal.h>
#include <fstream>
#include <iostream>

int main() {
    try {
        const auto require = [](bool ok, const char* message) {
            if (!ok) throw std::runtime_error(message);
        };
        const auto source_root = std::filesystem::path(__FILE__).parent_path().parent_path();
        const auto notices = ovmesh::license_notices();
        require(notices.size() >= 10, "Required component notices present");
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.LogFilename = nullptr;
        io.DisplaySize = {1000, 800}; io.DeltaTime = 1.0f / 60;
        unsigned char* pixels; int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ovmesh::Engine engine;
        ovmesh::DesktopState ui;
        ui.show_licenses = true;
        for (size_t i = 0; i < notices.size(); ++i) {
            std::ifstream file(source_root / notices[i].source_path, std::ios::binary);
            require(file.good(), "Notice source exists");
            const std::string original{std::istreambuf_iterator<char>(file), {}};
            require(notices[i].text == original, "Embedded license must preserve the entire original text");
            ui.selected_license = int(i);
            for (int frame = 0; frame < 2; ++frame) {
                ImGui::NewFrame();
                ovmesh::dialogs(engine, ui, engine.snapshot());
                ImGui::Render();
                const auto* context = ImGui::GetCurrentContext();
                require(context->CurrentWindowStack.empty(), "License dialog balances window/child scopes");
                require(ImGui::FindWindowByName("About & licenses") != nullptr, "About dialog rendered");
                const auto* draw = ImGui::GetDrawData();
                require(draw && draw->Valid && draw->TotalVtxCount > 0, "Notice renders visible geometry");
                for (int n = 0; n < draw->CmdListsCount; ++n)
                    for (const auto& v : draw->CmdLists[n]->VtxBuffer)
                        require(std::isfinite(v.pos.x) && std::isfinite(v.pos.y), "Notice geometry is finite");
            }
        }
        require(!engine.snapshot().running && !ui.preferences_active, "License browsing starts no receiver or profile");
        ImGui::DestroyContext();
        std::cout << "Embedded texts match all " << notices.size() << " source notices; dialogs render without devices\n";
        return 0;
    } catch (const std::exception& error) {
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
