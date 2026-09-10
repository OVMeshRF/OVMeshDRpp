// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/file_chooser.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
namespace fs = std::filesystem;
using ovmesh::FileChoiceKind;
void require(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
template<class F> void rejects(F operation, const char* message) {
    try { operation(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
std::string utf8(const fs::path& path) {
    const auto text = path.u8string(); return {text.begin(), text.end()};
}
struct Fixture {
    fs::path folder;
    Fixture() {
        // CTest runs under the repository build directory; avoid /tmp aliases
        // and keep fixtures inside the already authorized project location.
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        folder = fs::current_path() / ("file-chooser-fixture-" + std::to_string(nonce));
        require(fs::create_directory(folder), "Create unique fixture folder");
    }
    ~Fixture() { std::error_code error; fs::remove_all(folder, error); }
};
void touch(const fs::path& file) {
    std::ofstream output(file, std::ios::binary); output << "synthetic fixture";
    require(output.good(), "Write synthetic fixture");
}

void selection_and_no_writes(const fs::path& folder) {
    const auto directory = utf8(folder);
    const auto csv = ovmesh::choose_local_file(directory, "survey", FileChoiceKind::Csv, false);
    require(csv == utf8(folder / "survey.csv"), "Default CSV suffix");
    require(!fs::exists(folder / "survey.csv"), "Selecting a path must not create a file");
    require(ovmesh::choose_local_file(directory, "route", FileChoiceKind::GeoJson, false) ==
        utf8(folder / "route.geojson"), "Default GeoJSON suffix");
    require(ovmesh::choose_local_file(directory, "analysis", FileChoiceKind::Html, false) ==
        utf8(folder / "analysis.html"), "Default analysis HTML suffix");
    require(ovmesh::choose_local_file(directory, "Survey.SQLITE", FileChoiceKind::Survey, false) ==
        utf8(folder / "Survey.SQLITE"), "Preserve chosen filename case");
    const auto unicode = std::string("r\xc3\xa9gion-\xe6\xb8\xac\xe5\xae\x9a");
    const auto selected = ovmesh::choose_local_file(directory, unicode, FileChoiceKind::Survey, false);
    require(selected == utf8(folder / fs::path(std::u8string(unicode.begin(), unicode.end()) + u8".sqlite")),
        "Unicode filename must round-trip without locale conversion");
    touch(folder / "existing.sqlite");
    require(ovmesh::choose_local_file(directory, "existing.sqlite", FileChoiceKind::Survey, true) ==
        utf8(folder / "existing.sqlite"), "Open regular saved survey");
    for (const auto* name : {"legacy.db", "legacy-no-extension"}) {
        touch(folder / name);
        require(ovmesh::choose_local_file(directory, name, FileChoiceKind::Survey, true) == utf8(folder / name),
            "Existing surveys retain prechooser extension compatibility");
    }
    rejects([&] { ovmesh::choose_local_file(directory, "existing.sqlite", FileChoiceKind::Survey, false); },
        "Existing file must not be an overwrite candidate");
    rejects([&] { ovmesh::choose_local_file(directory, "missing.sqlite", FileChoiceKind::Survey, true); },
        "Open must require an existing file");
    fs::create_directory(folder / "directory.sqlite");
    rejects([&] { ovmesh::choose_local_file(directory, "directory.sqlite", FileChoiceKind::Survey, true); },
        "Cannot open a directory as survey");
    rejects([&] { ovmesh::choose_local_file(directory, "survey.geojson", FileChoiceKind::Csv, false); },
        "Cannot mislabel CSV output as GeoJSON");
    std::ifstream input(folder / "existing.sqlite", std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), {});
    require(contents == "synthetic fixture", "Choosing must not alter an existing file");
}

void hostile_names(const fs::path& folder) {
    const auto directory = utf8(folder);
    for (const auto& name : {"", ".", "..", "../escape", "dir/name", "dir\\name", "C:escape", "\\\\host\\share", "x\n.csv",
                             "name.", "name ", "a:b", "a?b", "a*b", "a|b", "a<b", "a>b", "a\"b", "CON", "nul.csv",
                             "COM1.csv", "LPT9.sqlite", "conin$.csv"})
        rejects([&] { ovmesh::choose_local_file(directory, name, FileChoiceKind::Csv, false); },
            "Unsafe or nonportable filename accepted");
    for (const std::string name : {std::string("nul\0.csv", 8), std::string("\xc0\xaf.csv"), std::string("\xed\xa0\x80.csv"), std::string("\xf4\x90\x80\x80.csv")})
        rejects([&] { ovmesh::choose_local_file(directory, name, FileChoiceKind::Csv, false); },
            "NUL or malformed UTF-8 accepted");
    rejects([&] { ovmesh::file_browser_join(directory, "../escape"); }, "Join permits traversal");
    rejects([&] { ovmesh::file_browser_join(directory, "nested/file"); }, "Join permits multiple components");
    rejects([&] { ovmesh::choose_local_file(directory + "/../", "escape", FileChoiceKind::Csv, false); },
        "Directory dot components must not be normalized away");
    require(!ovmesh::list_local_directory("relative").error.empty(), "Relative folder must fail before browsing");
    require(!ovmesh::list_local_directory("//invalid.invalid/share").error.empty(), "Network path must be rejected");
#ifndef _WIN32
    require(!ovmesh::list_local_directory("/Volumes").error.empty(), "Mounted-volume root must be rejected");
#endif
}

void listing_and_navigation(const fs::path& folder) {
    const auto directory = folder / "listing";
    fs::create_directory(directory);
    fs::create_directory(directory / "z-folder");
    touch(directory / "b.sqlite");
    touch(directory / "a%##.csv");
    const auto unicode = fs::path(u8"café.sqlite"); touch(directory / unicode);
    const auto listing = ovmesh::list_local_directory(utf8(directory));
    require(listing.error.empty() && !listing.truncated && listing.entries.size() == 4, "Complete folder listing");
    require(listing.entries[0].directory && listing.entries[0].name == "z-folder", "Folders sorted first");
    require(listing.entries[1].name == "a%##.csv", "Filename metacharacters must remain inert data");
    require(std::any_of(listing.entries.begin(), listing.entries.end(), [&](const auto& row) {
        return row.name == utf8(unicode) && !row.directory;
    }), "Unicode directory entry preserved");
    const auto bounded = ovmesh::list_local_directory(utf8(directory), 2);
    require(bounded.error.empty() && bounded.truncated && bounded.entries.size() == 2, "Listing truncation explicit");
    const auto zero = ovmesh::list_local_directory(utf8(directory), 0);
    require(zero.truncated && zero.entries.empty(), "Zero listing budget performs no entry inspection");
    require(ovmesh::file_browser_parent(utf8(directory)) == utf8(folder), "Parent navigation");
    require(ovmesh::file_browser_parent(utf8(directory) + "/") == utf8(folder), "Trailing separator navigation");
    require(ovmesh::file_browser_parent(utf8(folder.root_path())) == utf8(folder.root_path()), "Parent of root stays root");
    require(ovmesh::file_browser_join(utf8(directory), "z-folder") == utf8(directory / "z-folder"), "Folder join");
    require(!ovmesh::list_local_directory(utf8(directory / "b.sqlite")).error.empty(), "Cannot browse ordinary file");
    require(!ovmesh::list_local_directory(utf8(directory / "absent")).error.empty(), "Missing folder reported");
    require(!ovmesh::file_browser_roots().empty(), "At least one platform-local drive root");
}

void aliases_and_special_files(const fs::path& folder) {
    const auto real = folder / "real"; fs::create_directory(real); touch(real / "real.sqlite");
    std::error_code error;
    fs::create_directory_symlink(real, folder / "alias", error);
#ifdef _WIN32
    if (error) return; // Windows symlink creation may require developer mode.
#else
    require(!error, "Create local directory symlink fixture");
#endif
    require(!ovmesh::list_local_directory(utf8(folder / "alias")).error.empty(), "Do not follow folder alias");
    rejects([&] { ovmesh::choose_local_file(utf8(folder / "alias"), "real.sqlite", FileChoiceKind::Survey, true); },
        "Do not open through a parent symlink");
    fs::create_symlink(real / "real.sqlite", folder / "link.sqlite");
    fs::create_symlink(real / "missing.sqlite", folder / "dangling.sqlite");
    for (const auto* name : {"link.sqlite", "dangling.sqlite"}) {
        rejects([&] { ovmesh::choose_local_file(utf8(folder), name, FileChoiceKind::Survey, true); }, "Do not open symlink");
        rejects([&] { ovmesh::choose_local_file(utf8(folder), name, FileChoiceKind::Survey, false); }, "Do not replace symlink");
    }
#ifndef _WIN32
    require(::mkfifo((folder / "pipe.sqlite").c_str(), 0600) == 0, "Create synthetic FIFO");
    rejects([&] { ovmesh::choose_local_file(utf8(folder), "pipe.sqlite", FileChoiceKind::Survey, true); }, "Do not open special file");
#endif
    const auto listing = ovmesh::list_local_directory(utf8(folder));
    require(listing.error.empty(), "Safe listing of aliases/special files");
    for (const auto& row : listing.entries)
        require(row.name != "alias" && row.name != "link.sqlite" && row.name != "dangling.sqlite" && row.name != "pipe.sqlite",
            "Aliases or special files must not be offered");
}

void home_fallback(const fs::path& folder) {
#ifdef _WIN32
    const wchar_t* old = _wgetenv(L"USERPROFILE");
    const bool existed = old != nullptr;
    const std::wstring previous = old ? old : L"";
    struct Restore {
        bool existed;
        std::wstring previous;
        ~Restore() { _wputenv_s(L"USERPROFILE", existed ? previous.c_str() : L""); }
    } restore{existed, previous};
    require(_wputenv_s(L"USERPROFILE", folder.c_str()) == 0, "Set fixture-local home");
    require(ovmesh::file_browser_home() == utf8(folder), "Choose the local Windows home");
    require(_wputenv_s(L"USERPROFILE", L"\\\\invalid.invalid\\share") == 0, "Set rejected home fixture");
#else
    const char* old = std::getenv("HOME");
    const bool existed = old != nullptr;
    const std::string previous = old ? old : "";
    struct Restore {
        bool existed;
        std::string previous;
        ~Restore() { if (existed) ::setenv("HOME", previous.c_str(), 1); else ::unsetenv("HOME"); }
    } restore{existed, previous};
    require(::setenv("HOME", folder.c_str(), 1) == 0, "Set fixture-local home");
    require(ovmesh::file_browser_home() == utf8(folder), "Choose the local POSIX home");
    require(::setenv("HOME", "//invalid.invalid/share", 1) == 0, "Set rejected home fixture");
#endif
    require(ovmesh::file_browser_home() == utf8(fs::current_path()),
        "A prohibited home falls back to a validated local working folder");
}
} // namespace

int main() {
    try {
        Fixture fixture;
        selection_and_no_writes(fixture.folder);
        hostile_names(fixture.folder);
        listing_and_navigation(fixture.folder);
        aliases_and_special_files(fixture.folder);
        home_fallback(fixture.folder);
        std::cout << "File chooser checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "File chooser checks failed: " << e.what() << '\n';
        return 1;
    }
}
