// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/file_chooser.hpp"
#include "ovmesh/local_paths.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <cerrno>
#include <chrono>
#include <thread>
extern char **environ;
#endif

namespace ovmesh {
namespace {
namespace fs = std::filesystem;

bool valid_utf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        unsigned value = 0, remaining = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { value = first & 0x1f; remaining = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { value = first & 0x0f; remaining = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { value = first & 7; remaining = 3; minimum = 0x10000; }
        else return false;
        if (remaining > text.size() - i) return false;
        for (unsigned n = 0; n < remaining; ++n) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

fs::path from_utf8(const std::string& value) {
    if (value.find('\0') != std::string::npos || !valid_utf8(value))
        throw std::runtime_error("The selected path is not valid UTF-8 text");
    return fs::path(std::u8string(value.begin(), value.end()));
}

std::string to_utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

fs::path checked_directory(const std::string& value) {
    auto directory = from_utf8(value);
    if (directory.empty() || !directory.is_absolute())
        throw std::runtime_error("Choose an absolute local folder");
    // Remove only a trailing separator, never dot components or aliases.
    while (directory != directory.root_path() && directory.filename().empty())
        directory = directory.parent_path();
    validate_local_file_path(to_utf8(directory / ".ovmesh-chooser-path-check"));
    std::error_code error;
    const auto status = fs::symlink_status(directory, error);
    if (error || !fs::is_directory(status))
        throw std::runtime_error("The selected local folder is unavailable");
    return directory;
}

void check_component(const std::string& name) {
    const auto path = from_utf8(name);
    if (name.empty() || name == "." || name == ".." ||
        name.find_first_of("/\\") != std::string::npos || path.has_root_path() || path.filename() != path)
        throw std::runtime_error("Enter a filename without a folder or path separator");
#ifdef _WIN32
    if (name.find(':') != std::string::npos)
        throw std::runtime_error("Alternate data streams are not supported for survey files");
#endif
}

std::string ascii_lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return text;
}

void check_portable_filename(const std::string& name) {
    check_component(name);
    if (name.find_first_of("<>:\"|?*") != std::string::npos || name.back() == '.' || name.back() == ' ' ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::runtime_error("Choose a filename without reserved characters, controls, or a trailing dot/space");
    const auto base = ascii_lower(name.substr(0, name.find('.')));
    bool device = base == "con" || base == "prn" || base == "aux" || base == "nul" ||
                  base == "conin$" || base == "conout$" || base == "clock$";
    if (base.starts_with("com") || base.starts_with("lpt")) {
        const auto number = base.substr(3);
        device = device || (number.size() == 1 && number[0] >= '1' && number[0] <= '9') ||
                 number == "\xc2\xb9" || number == "\xc2\xb2" || number == "\xc2\xb3";
    }
    if (device) throw std::runtime_error("Choose a filename that is not a reserved device name");
}

const char* suffix(FileChoiceKind kind) {
    switch (kind) {
        case FileChoiceKind::Survey: return ".sqlite";
        case FileChoiceKind::Csv: return ".csv";
        case FileChoiceKind::GeoJson: return ".geojson";
        case FileChoiceKind::Png: return ".png";
        case FileChoiceKind::Html: return ".html";
    }
    throw std::runtime_error("Choose a supported file format");
}

bool reparse_point(const fs::path& path) {
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    (void)path;
    return false;
#endif
}
} // namespace

std::string file_browser_home() {
    try {
#ifdef _WIN32
        if (const wchar_t* value = _wgetenv(L"USERPROFILE"); value && *value)
            return to_utf8(checked_directory(to_utf8(fs::path(value))));
#else
        if (const char* value = std::getenv("HOME"); value && *value)
            return to_utf8(checked_directory(value));
#endif
    } catch (const std::exception&) { /* A redirected home must not be followed. */ }
    try { return to_utf8(checked_directory(to_utf8(fs::current_path()))); }
    catch (const std::exception&) { throw std::runtime_error("No local starting folder is available"); }
}

std::vector<std::string> file_browser_roots() {
#ifdef _WIN32
    std::vector<std::string> roots;
    const DWORD drives = GetLogicalDrives();
    for (unsigned i = 0; i < 26; ++i) {
        if ((drives & (DWORD{1} << i)) == 0) continue;
        const wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0'};
        // Query drive type only; never enumerate or stat a remote/removable drive.
        if (GetDriveTypeW(root) == DRIVE_FIXED) roots.push_back(to_utf8(fs::path(root)));
    }
    return roots;
#else
    return {"/"};
#endif
}

std::string file_browser_parent(const std::string& directory) {
    const auto current = checked_directory(directory);
    const auto parent = current == current.root_path() ? current : current.parent_path();
    return to_utf8(checked_directory(to_utf8(parent)));
}

std::string file_browser_join(const std::string& directory, const std::string& name) {
    check_component(name);
    return to_utf8(checked_directory(directory) / from_utf8(name));
}

FileBrowserListing list_local_directory(const std::string& directory, size_t limit) {
    FileBrowserListing result;
    try {
        const auto folder = checked_directory(directory);
        std::error_code error;
        fs::directory_iterator cursor(folder, error), end;
        if (error) throw std::runtime_error("The selected folder cannot be read");
        size_t inspected = 0;
        // Bound inspected entries, not merely accepted rows: a folder full of
        // links or special files must not cause unbounded synchronous work.
        while (cursor != end) {
            if (inspected == limit) { result.truncated = true; break; }
            ++inspected;
            const auto status = cursor->symlink_status(error);
            if (error) throw std::runtime_error("A folder entry could not be inspected; refresh to retry");
            if (!fs::is_symlink(status) && (fs::is_directory(status) || fs::is_regular_file(status)) &&
                !reparse_point(cursor->path())) {
                const auto name = to_utf8(cursor->path().filename());
                if (valid_utf8(name)) result.entries.push_back({name, fs::is_directory(status)});
            }
            cursor.increment(error);
            if (error) throw std::runtime_error("Folder listing was interrupted; refresh to retry");
        }
        std::sort(result.entries.begin(), result.entries.end(), [](const auto& a, const auto& b) {
            if (a.directory != b.directory) return a.directory;
            return a.name < b.name;
        });
    } catch (const std::exception& e) {
        result.entries.clear();
        result.error = e.what();
    }
    return result;
}

std::string choose_local_file(const std::string& directory, const std::string& filename,
                             FileChoiceKind kind, bool must_exist) {
    if (must_exist) check_component(filename);
    else check_portable_filename(filename);
    auto name = from_utf8(filename);
    if (!must_exist) {
        const std::string expected = suffix(kind);
        if (name.extension().empty()) name += expected;
        else if (ascii_lower(to_utf8(name.extension())) != expected)
            throw std::runtime_error("The filename extension must match the selected " + expected + " format");
    }
    const auto file = checked_directory(directory) / name;
    const auto path = to_utf8(file);
    validate_local_file_path(path);
    std::error_code error;
    const auto status = fs::symlink_status(file, error);
    if (error && error != std::errc::no_such_file_or_directory)
        throw std::runtime_error("The selected file cannot be inspected");
    if (must_exist) {
        if (error || !fs::is_regular_file(status) || reparse_point(file))
            throw std::runtime_error("Choose an existing local survey file");
    } else if (fs::exists(status)) {
        throw std::runtime_error("That filename already exists; choose a new filename");
    }
    return path;
}

void open_local_report(const std::string& path) {
    const auto file = from_utf8(path);
    if (ascii_lower(to_utf8(file.extension())) != ".html")
        throw std::runtime_error("Only local HTML analysis reports can be opened");
    const auto checked = choose_local_file(to_utf8(file.parent_path()), to_utf8(file.filename()), FileChoiceKind::Html, true);
#ifdef _WIN32
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        throw std::runtime_error("Windows could not initialize the report opener");
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (result <= 32) throw std::runtime_error("Windows could not open the report in the default browser");
#else
#ifdef __APPLE__
    const char* launcher = "/usr/bin/open";
#else
    const char* launcher = "/usr/bin/xdg-open";
#endif
    // An absolute checked filename cannot be interpreted as an option or URL.
    char* args[] = {const_cast<char*>(launcher), const_cast<char*>(checked.c_str()), nullptr};
    pid_t child = 0;
    if (posix_spawn(&child, launcher, nullptr, nullptr, args, environ) != 0)
        throw std::runtime_error("Cannot launch the default browser; open the saved HTML file manually");
    int status = 0;
    pid_t result = 0;
    for (unsigned attempt = 0; attempt < 50; ++attempt) {
        do { result = waitpid(child, &status, WNOHANG); } while (result < 0 && errno == EINTR);
        if (result != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (result == 0) {
        // Some Linux handlers stay attached to the browser. Reap them without
        // keeping the desktop's report operation busy until the browser closes.
        std::thread([child] {
            int exit_status = 0;
            while (waitpid(child, &exit_status, 0) < 0 && errno == EINTR) {}
        }).detach();
        return;
    }
    if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error("The default browser could not open the report; open the saved HTML file manually");
#endif
}

} // namespace ovmesh
