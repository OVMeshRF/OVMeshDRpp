// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual desktop chooser with an in-memory ImGui context. This
// test does not initialize GLFW, create an OS window, or open a radio/GPS.
#include "../src/ui.cpp"
#include <imgui_internal.h>
#include <fstream>
#include <iostream>

namespace {
namespace fs = std::filesystem;
using namespace ovmesh;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    fs::path folder;
    Fixture() {
        folder = fs::current_path() / ("file-chooser-ui-fixture-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(fs::create_directory(folder), "Create unique local fixture folder");
        fs::create_directory(folder / "nested");
        std::ofstream file(folder / "legacy-survey.db", std::ios::binary);
        file << "synthetic fixture, deliberately not a survey database";
        require(file.good(), "Write synthetic existing file");
    }
    ~Fixture() { std::error_code error; fs::remove_all(folder, error); }
    std::string path(const char* filename) const { return path_utf8(folder / filename); }
    size_t count() const {
        return static_cast<size_t>(std::distance(fs::directory_iterator(folder), fs::directory_iterator{}));
    }
};

struct HeadlessUi {
    HeadlessUi() {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = {1000, 700};
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        require(pixels != nullptr && width > 0 && height > 0, "Build in-memory font atlas");
        io.AddFocusEvent(true);
    }
    ~HeadlessUi() { ImGui::DestroyContext(); }
    void frame(DesktopState& ui, const Snapshot& snapshot) {
        ImGui::NewFrame();
        file_picker_dialog(ui, snapshot);
        ImGui::Render();
        require(ImGui::GetCurrentContext()->CurrentWindowStack.empty(), "Window stack balanced after modal frame");
        require(ImGui::GetCurrentContext()->BeginPopupStack.empty(), "Popup stack balanced after modal frame");
    }
    void settle(DesktopState& ui, const Snapshot& snapshot) { frame(ui, snapshot); frame(ui, snapshot); }
    void key(DesktopState& ui, const Snapshot& snapshot, ImGuiKey key) {
        ImGui::GetIO().AddKeyEvent(key, true);
        frame(ui, snapshot);
        ImGui::GetIO().AddKeyEvent(key, false);
        frame(ui, snapshot);
    }
    void activate(DesktopState& ui, const Snapshot& snapshot, const char* label) {
        auto* window = ImGui::FindWindowByName("Choose a local file");
        require(window != nullptr && window->Active, "Chooser window is active before button navigation");
        const auto target = window->GetID(label);
        for (unsigned attempt = 0; attempt < 64 && ImGui::GetCurrentContext()->NavId != target; ++attempt)
            key(ui, snapshot, ImGuiKey_Tab);
        require(ImGui::GetCurrentContext()->NavId == target,
                std::string("Keyboard navigation reaches actual control: ") + label);
        key(ui, snapshot, ImGuiKey_Enter);
    }
};

void current_file_and_cancel(HeadlessUi& harness, const Fixture& fixture) {
    DesktopState ui;
    Snapshot snapshot;
    ui.export_path = fixture.path("regional.GEOJSON");
    ui.export_options.include_content = true;
    ui.export_options.include_provenance = false;
    ui.export_options.include_receiver_positions = false;
    ui.export_options.coordinate_decimals = 3;
    const auto original = ui.export_path;
    const auto before = fixture.count();
    begin_file_picker(ui, FilePickerPurpose::Export, ui.export_path);
    require(ui.file_picker.error.empty(), "Selected new filename initializes its parent folder");
    require(ui.file_picker.directory == path_utf8(fixture.folder), "Selected file parent is used");
    require(std::string(ui.file_picker.filename.data()) == "regional.GEOJSON" && ui.file_picker.format == 1,
            "Existing export selection preserves filename and recognizes uppercase GeoJSON");
    harness.settle(ui, snapshot);
    copy_text(ui.file_picker.filename, "cancelled.geojson");
    harness.activate(ui, snapshot, "Cancel");
    require(ui.file_picker.purpose == FilePickerPurpose::None, "Cancel closes chooser");
    require(ui.export_path == original && fixture.count() == before, "Cancel preserves selected path and creates no files");
    require(ui.export_options.include_content && !ui.export_options.include_provenance &&
            !ui.export_options.include_receiver_positions && ui.export_options.coordinate_decimals == 3,
            "Choosing/cancelling GeoJSON does not change independent privacy options");
}

void choose_export_and_recording(HeadlessUi& harness, const Fixture& fixture) {
    DesktopState ui;
    Snapshot snapshot;
    const auto before = fixture.count();
    begin_file_picker(ui, FilePickerPurpose::Export, "", fixture.path("legacy-survey.db"));
    require(ui.file_picker.error.empty() && ui.file_picker.directory == path_utf8(fixture.folder),
            "Existing survey file supplies export parent folder without treating the file as a directory");
    require(std::string(ui.file_picker.filename.data()) == "survey.csv", "New export offers a filename");
    harness.settle(ui, snapshot);
    harness.activate(ui, snapshot, "Choose file");
    require(ui.export_path == fixture.path("survey.csv") && ui.file_picker.purpose == FilePickerPurpose::None,
            "Actual Choose button selects new CSV destination");
    require(fixture.count() == before && !fs::exists(fixture.folder / "survey.csv"),
            "Choose destination does not export or create a file");
    require(!ui.export_options.include_content && !ui.export_options.include_provenance &&
            !ui.export_options.include_receiver_positions, "Export chooser preserves private defaults");

    begin_file_picker(ui, FilePickerPurpose::SaveSurvey, fixture.path("new-recording.sqlite"));
    harness.settle(ui, snapshot);
    harness.activate(ui, snapshot, "Choose file");
    require(ui.session_path == fixture.path("new-recording.sqlite"), "Recording chooser selects destination");
    require(ui.config.session_path.empty() && !fs::exists(fixture.folder / "new-recording.sqlite"),
            "Recording selection does not start or create a session");
}

void recover_missing_recording_folder(HeadlessUi& harness, const Fixture& fixture) {
    DesktopState ui;
    Snapshot snapshot;
    const auto profile = fixture.path("chooser-recovery-profile");
    ui.initialize_preferences(profile, false); // Explicit fixture only; no OS device discovery.
    require(ui.preferences_ready && ui.preferences_error.empty(), "Initialize isolated chooser preferences");
    ui.preferences.recording_directory = fixture.path("missing-original-folder");
    ui.persist_preferences();
    ui.prepare_recording_file();
    require(ui.notice_error && ui.session_path.empty(), "Unavailable recording folder produces the prior error");
    const auto failed_notice = ui.notice;
    const auto before = fixture.count();

    begin_file_picker(ui, FilePickerPurpose::SaveSurvey, fixture.path("recovered-recording.sqlite"));
    harness.settle(ui, snapshot);
    harness.activate(ui, snapshot, "Choose file");
    require(ui.file_picker.purpose == FilePickerPurpose::None &&
            ui.session_path == fixture.path("recovered-recording.sqlite"), "Actual chooser accepts a valid replacement destination");
    require(!ui.notice_error && ui.notice != failed_notice && ui.preferences_error.empty(),
            "Successful choice replaces the stale missing-folder error with confirmation");
    require(load_preferences(ui.preference_locations).recording_directory == path_utf8(fixture.folder),
            "Replacement recording folder is saved to the isolated preferences");
    require(!fs::exists(fixture.folder / "recovered-recording.sqlite") && ui.config.session_path.empty() &&
            fixture.count() == before, "Recovering the location creates no survey file or active recording");
    DesktopState reopened;
    reopened.initialize_preferences(profile, false);
    require(reopened.preferences_ready && !reopened.notice_error &&
            reopened.preferences.recording_directory == path_utf8(fixture.folder),
            "Restart uses the recovered folder without repeating the prior error");
}

void legacy_open_and_overwrite_refusal(HeadlessUi& harness, const Fixture& fixture) {
    DesktopState ui;
    Snapshot snapshot;
    const auto before = fixture.count();
    begin_file_picker(ui, FilePickerPurpose::OpenSurvey, fixture.path("legacy-survey.db"));
    require(ui.file_picker.error.empty(), "Existing saved file initializes chooser successfully");
    harness.settle(ui, snapshot);
    harness.activate(ui, snapshot, "Choose file");
    require(ui.reopen_path == fixture.path("legacy-survey.db"), "Legacy saved extension can be selected");
    require(snapshot.session_id.empty() && fixture.count() == before, "Choosing does not open the intentionally invalid fixture");

    ui.export_path = fixture.path("previous-selection.csv");
    // Existing files cannot be selected as new export destinations. Use a .csv
    // fixture so refusal comes from no-overwrite, rather than suffix validation.
    { std::ofstream file(fixture.folder / "existing.csv"); file << "unchanged"; }
    begin_file_picker(ui, FilePickerPurpose::Export, ui.export_path);
    copy_text(ui.file_picker.filename, "existing.csv");
    harness.settle(ui, snapshot);
    harness.activate(ui, snapshot, "Choose file");
    require(!ui.file_picker.error.empty() && ui.file_picker.purpose == FilePickerPurpose::Export,
            "Choose keeps modal open and reports existing-file refusal");
    require(ui.export_path == fixture.path("previous-selection.csv"), "Failed choice preserves previous selection");
    { std::ifstream file(fixture.folder / "existing.csv"); std::string text; file >> text;
      require(text == "unchanged", "Existing destination content is unchanged"); }
    harness.activate(ui, snapshot, "Cancel");
}

void navigation_and_cancel(HeadlessUi& harness, const Fixture& fixture) {
    DesktopState ui;
    Snapshot snapshot;
    begin_file_picker(ui, FilePickerPurpose::OpenSurvey, fixture.path("legacy-survey.db"));
    harness.settle(ui, snapshot);
    browse_folder(ui.file_picker, path_utf8(fixture.folder / "nested"));
    require(ui.file_picker.filename[0] == 0, "Changing folder clears stale open filename");
    harness.settle(ui, snapshot);
    harness.activate(ui, snapshot, "Up");
    require(ui.file_picker.directory == path_utf8(fixture.folder), "Actual Up button navigates to parent");
    harness.activate(ui, snapshot, "Cancel");
    require(ui.file_picker.purpose == FilePickerPurpose::None && ui.reopen_path.empty(),
            "Navigation and cancellation do not select a stale filename");
}

void short_viewport(HeadlessUi& harness, const Fixture& fixture) {
    DesktopState ui;
    Snapshot snapshot;
    ImGui::GetIO().DisplaySize = {900, 360};
    ImGui::GetIO().FontGlobalScale = 1.7f;
    begin_file_picker(ui, FilePickerPurpose::Export, fixture.path("small-screen.csv"));
    harness.settle(ui, snapshot);
    auto* window = ImGui::FindWindowByName("Choose a local file");
    require(window && window->Size.y <= 330.5f, "Modal remains within short viewport");
    harness.activate(ui, snapshot, "Cancel");
    require(ui.file_picker.purpose == FilePickerPurpose::None, "Cancel reachable with short viewport and large text");
    ImGui::GetIO().DisplaySize = {1000, 700};
    ImGui::GetIO().FontGlobalScale = 1;
}
} // namespace

int main() {
    try {
        Fixture fixture;
        HeadlessUi harness;
        current_file_and_cancel(harness, fixture);
        choose_export_and_recording(harness, fixture);
        recover_missing_recording_folder(harness, fixture);
        legacy_open_and_overwrite_refusal(harness, fixture);
        navigation_and_cancel(harness, fixture);
        short_viewport(harness, fixture);
        std::cout << "Actual file chooser UI control checks passed; no windows or USB opened\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "File chooser UI checks failed: " << error.what() << '\n';
        return 1;
    }
}
