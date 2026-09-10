// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/preferences.hpp"
#include "ovmesh/local_paths.hpp"

#include <openssl/rand.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ovmesh {
namespace {
namespace fs = std::filesystem;
constexpr size_t maximum_settings_size = 32768;
constexpr size_t maximum_path_size = 4096;
constexpr size_t maximum_device_id_size = 4096;

bool valid_text(const std::string& text, size_t limit) {
    if (text.size() > limit) return false;
    for (size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x20 || first == 0x7f) return false;
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

std::string utf8(const fs::path& path) {
    const auto value = path.u8string(); return {value.begin(), value.end()};
}

fs::path path_from(const std::string& value) {
    if (!valid_text(value, maximum_path_size)) throw std::runtime_error("Settings path is invalid or too long");
    return fs::path(std::u8string(value.begin(), value.end()));
}

fs::path absolute_directory(const std::string& value) {
    auto directory = path_from(value);
    if (directory.empty() || !directory.is_absolute() || value.starts_with("//") || value.starts_with("\\\\"))
        throw std::runtime_error("Choose an absolute local settings directory");
    for (const auto& component : directory) {
        if (component == "." || component == "..") throw std::runtime_error("Settings paths cannot contain dot components");
#ifdef _WIN32
        if (component != directory.root_name() && component.native().find(L':') != std::wstring::npos)
            throw std::runtime_error("Alternate data streams are not supported for settings");
#endif
    }
    while (directory != directory.root_path() && directory.filename().empty()) directory = directory.parent_path();
    if (directory == directory.root_path()) throw std::runtime_error("Choose an application settings folder");
    return directory;
}

void check_paths(const PreferencePaths& paths) {
    const auto directory = absolute_directory(paths.directory);
    if (path_from(paths.settings_file) != directory / "preferences.conf" ||
        path_from(paths.surveys_directory) != directory / "Surveys")
        throw std::runtime_error("Invalid application preference paths");
}

bool missing(const std::error_code& error, const fs::file_status& status) {
    return error == std::errc::no_such_file_or_directory || (!error && status.type() == fs::file_type::not_found);
}

// Check each existing component before looking farther down the path. This
// does not resolve symlinks, aliases, or a missing network-mounted ancestor.
bool directory_exists(const fs::path& directory) {
    fs::path prefix = directory.root_path();
    for (const auto& component : directory.relative_path()) {
        prefix /= component;
        validate_local_file_path(utf8(prefix));
        std::error_code error;
        const auto status = fs::symlink_status(prefix, error);
        if (missing(error, status)) return false;
        if (error || !fs::is_directory(status)) throw std::runtime_error("Settings parent is unavailable or is not a directory");
        validate_local_file_path(utf8(prefix / ".ovmesh-directory-check"));
    }
    return true;
}

#ifdef _WIN32
class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

class PrivateSecurity {
public:
    PrivateSecurity() {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &descriptor_, nullptr))
            throw std::runtime_error("Cannot prepare private settings permissions");
        attributes_ = {sizeof(SECURITY_ATTRIBUTES), descriptor_, FALSE};
    }
    ~PrivateSecurity() { if (descriptor_) LocalFree(descriptor_); }
    SECURITY_ATTRIBUTES* attributes() { return &attributes_; }
private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};

void verify_private_handle(HANDLE handle, bool directory) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory || (!directory && info.nNumberOfLinks != 1))
        throw std::runtime_error("Settings require a regular private file or directory");
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr; PACL acl = nullptr;
    if (GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot inspect settings permissions");
    struct Release { PSECURITY_DESCRIPTOR descriptor; ~Release() { LocalFree(descriptor); } } release{descriptor};
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) throw std::runtime_error("Cannot inspect settings owner");
    Handle token(raw_token);
    DWORD size = 0; GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (size == 0 || size > 65536) throw std::runtime_error("Cannot inspect settings owner");
    std::vector<unsigned char> buffer(size);
    if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) throw std::runtime_error("Cannot inspect settings owner");
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    if (!owner || !EqualSid(owner, user->User.Sid) || !acl) throw std::runtime_error("Settings must be private to the current user");
    PSID owner_rights = nullptr;
    if (!ConvertStringSidToSidW(L"S-1-3-4", &owner_rights)) throw std::runtime_error("Cannot inspect private settings permissions");
    struct ReleaseSid { PSID sid; ~ReleaseSid() { LocalFree(sid); } } release_sid{owner_rights};
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void* value = nullptr;
        if (!GetAce(acl, index, &value)) throw std::runtime_error("Cannot inspect private settings permissions");
        const auto* header = static_cast<const ACE_HEADER*>(value);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) throw std::runtime_error("Unsupported settings permissions");
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(value);
        auto sid = const_cast<DWORD*>(&ace->SidStart);
        if (ace->Mask != 0 && !EqualSid(sid, user->User.Sid) && !EqualSid(sid, owner_rights))
            throw std::runtime_error("Settings must be private to the current user");
    }
}

void verify_private_directory(const fs::path& directory) {
    Handle handle(CreateFileW(directory.c_str(), READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot inspect settings directory");
    verify_private_handle(handle.get(), true);
}
#else
class Descriptor {
public:
    explicit Descriptor(int value) : value_(value) {}
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return value_; }
private:
    int value_;
};

void verify_private_stat(const struct stat& status, bool directory) {
    if ((directory ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode)) ||
        status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0 || (!directory && status.st_nlink != 1))
        throw std::runtime_error("Settings must be an owner-only directory (0700) or file (0600)");
}

void verify_private_directory(const fs::path& directory) {
    struct stat status{};
    if (::lstat(directory.c_str(), &status) != 0) throw std::runtime_error("Cannot inspect settings directory");
    verify_private_stat(status, true);
}
#endif

void create_directory_tree(const fs::path& directory) {
    fs::path prefix = directory.root_path();
    for (const auto& component : directory.relative_path()) {
        prefix /= component;
        validate_local_file_path(utf8(prefix));
        std::error_code error;
        auto status = fs::symlink_status(prefix, error);
        if (missing(error, status)) {
#ifdef _WIN32
            PrivateSecurity security;
            if (!CreateDirectoryW(prefix.c_str(), security.attributes()) && GetLastError() != ERROR_ALREADY_EXISTS)
                throw std::runtime_error("Cannot create application settings directory");
#else
            if (::mkdir(prefix.c_str(), 0700) != 0 && errno != EEXIST)
                throw std::runtime_error("Cannot create application settings directory");
#endif
            validate_local_file_path(utf8(prefix / ".ovmesh-directory-check"));
            status = fs::symlink_status(prefix, error);
        }
        if (error || !fs::is_directory(status)) throw std::runtime_error("Settings parent is unavailable or is not a directory");
        validate_local_file_path(utf8(prefix / ".ovmesh-directory-check"));
    }
    verify_private_directory(directory);
}

std::string hex_encode(const std::string& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded; encoded.reserve(value.size() * 2);
    for (const char value_byte : value) {
        const auto byte = static_cast<unsigned char>(value_byte);
        encoded += digits[byte >> 4]; encoded += digits[byte & 15];
    }
    return encoded;
}

std::string hex_decode(const std::string& value, size_t limit) {
    if (value.size() > limit * 2 || value.size() % 2 != 0) throw std::runtime_error("Invalid preference text field");
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
        throw std::runtime_error("Invalid preference text encoding");
    };
    std::string decoded; decoded.reserve(value.size() / 2);
    for (size_t i = 0; i < value.size(); i += 2) decoded += static_cast<char>((digit(value[i]) << 4) | digit(value[i + 1]));
    if (!valid_text(decoded, limit)) throw std::runtime_error("Invalid preference text field");
    return decoded;
}

void validate_preferences(const DesktopPreferences& preferences) {
    absolute_directory(preferences.recording_directory);
    if (!valid_text(preferences.gps_device_id, maximum_device_id_size)) throw std::runtime_error("Invalid GPS device preference");
    switch (preferences.gps_baud) {
        case 4800: case 9600: case 19200: case 38400: case 57600: case 115200: break;
        default: throw std::runtime_error("Unsupported GPS baud preference");
    }
    if (preferences.tuning_offset_hz < -100000 || preferences.tuning_offset_hz > 100000)
        throw std::runtime_error("Receiver tuning offset must be between -100000 and 100000 Hz");
    if (preferences.receiver_source != DesktopReceiver::Synthetic && preferences.receiver_source != DesktopReceiver::HackRf &&
        preferences.receiver_source != DesktopReceiver::RtlSdr)
        throw std::runtime_error("Unsupported preferred receiver");
    if (preferences.center_hz < 1000000 || preferences.center_hz > 6000000000ULL)
        throw std::runtime_error("Preferred center frequency must be between 1 MHz and 6 GHz");
    const auto corrected = static_cast<int64_t>(preferences.center_hz) + preferences.tuning_offset_hz;
    if (corrected < 1000000 || corrected > 6000000000LL)
        throw std::runtime_error("Preferred offset moves the tuner outside its range");
    if (preferences.receiver_source == DesktopReceiver::RtlSdr) {
        if (preferences.center_hz < 24000000 || preferences.center_hz > 1766000000ULL ||
            corrected < 24000000 || corrected > 1766000000LL)
            throw std::runtime_error("Preferred RTL-SDR center and corrected frequency must be between 24 and 1766 MHz");
        if (preferences.sample_rate != 1000000 && preferences.sample_rate != 2000000)
            throw std::runtime_error("RTL-SDR preferred sample rate must be 1 or 2 MS/s");
        if (preferences.amplifier) throw std::runtime_error("RTL-SDR has no HackRF RF amplifier control");
    } else switch (preferences.sample_rate) {
        case 8000000: case 10000000: case 12000000: case 16000000: case 20000000: break;
        default: throw std::runtime_error("Unsupported preferred sample rate");
    }
    if (preferences.survey_span_hz < 500000 || preferences.survey_span_hz > preferences.sample_rate * 4 / 5)
        throw std::runtime_error("Preferred survey span must be 0.5 MHz through 80% of sample rate");
    if (preferences.center_hz < preferences.survey_span_hz / 2 ||
        preferences.center_hz + preferences.survey_span_hz / 2 > 6000000000ULL)
        throw std::runtime_error("Preferred survey edges exceed receiver range");
    if (preferences.lna_gain > 40 || preferences.lna_gain % 8 || preferences.vga_gain > 62 || preferences.vga_gain % 2)
        throw std::runtime_error("Preferred LNA must be 0-40 dB in 8 dB steps; VGA 0-62 dB in 2 dB steps");
    if (preferences.rtl_gain_tenths_db < -100 || preferences.rtl_gain_tenths_db > 600)
        throw std::runtime_error("Preferred RTL-SDR tuner gain must be -10 through 60 dB");
}

DesktopPreferences parse(const std::string& text) {
    if (text.empty() || text.size() > maximum_settings_size || text.back() != '\n')
        throw std::runtime_error("Incomplete or oversized desktop settings");
    std::map<std::string, std::string> fields;
    size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        const auto equal = text.find('=', start);
        if (end == std::string::npos || equal == std::string::npos || equal >= end || equal == start || fields.size() >= 22)
            throw std::runtime_error("Malformed desktop settings");
        if (!fields.emplace(text.substr(start, equal - start), text.substr(equal + 1, end - equal - 1)).second)
            throw std::runtime_error("Duplicate desktop preference");
        start = end + 1;
    }
    for (const auto* name : {"version", "gps_enabled", "recording_enabled", "recording_directory", "gps_device_id", "gps_baud"})
        if (!fields.contains(name)) throw std::runtime_error("Missing desktop preference");
    const auto& version = fields.at("version");
    if (version != "1" && version != "2" && version != "3" && version != "4" && version != "5" && version != "6")
        throw std::runtime_error("Unsupported desktop settings version");
    if ((version == "1" && fields.size() != 6) ||
        (version == "2" && (fields.size() != 7 || !fields.contains("tuning_offset_hz"))) ||
        (version == "3" && (fields.size() != 8 || !fields.contains("tuning_offset_hz") || !fields.contains("discover_lora"))) ||
        (version == "4" && (fields.size() != 9 || !fields.contains("tuning_offset_hz") ||
            !fields.contains("discover_lora") || !fields.contains("compact_recording"))))
        throw std::runtime_error("Missing or unknown desktop preference");
    if (version == "5" || version == "6") {
        if (fields.size() != (version == "6" ? 22 : 20)) throw std::runtime_error("Missing or unknown desktop preference");
        for (const auto* name : {"tuning_offset_hz", "discover_lora", "compact_recording", "spectrum_only", "receiver_source",
                "center_hz", "sample_rate", "survey_span_hz", "lna_gain", "vga_gain", "amplifier", "mixed_fonts",
                "mobile_position_display", "decode_enabled"})
            if (!fields.contains(name)) throw std::runtime_error("Missing or unknown desktop preference");
        if (version == "6" && (!fields.contains("rtl_gain_tenths_db") || !fields.contains("rtl_auto_gain")))
            throw std::runtime_error("Missing RTL-SDR desktop preference");
    }
    auto boolean = [&](const char* name) {
        const auto& value = fields.at(name);
        if (value != "0" && value != "1") throw std::runtime_error("Invalid desktop preference switch");
        return value == "1";
    };
    auto integer = [&](const char* name, auto& result) {
        const auto& value = fields.at(name);
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || std::to_string(result) != value)
            throw std::runtime_error("Invalid numeric desktop preference");
    };
    DesktopPreferences result;
    result.gps_enabled = boolean("gps_enabled"); result.recording_enabled = boolean("recording_enabled");
    if (version == "3" || version == "4" || version == "5" || version == "6") result.discover_lora = boolean("discover_lora");
    if (version == "4" || version == "5" || version == "6") result.compact_recording = boolean("compact_recording");
    result.recording_directory = hex_decode(fields.at("recording_directory"), maximum_path_size);
    result.gps_device_id = hex_decode(fields.at("gps_device_id"), maximum_device_id_size);
    const auto& baud = fields.at("gps_baud");
    if (baud.empty() || baud.size() > 6 || baud.front() == '0') throw std::runtime_error("Invalid GPS baud preference");
    result.gps_baud = 0;
    for (const char digit : baud) {
        if (digit < '0' || digit > '9') throw std::runtime_error("Invalid GPS baud preference");
        result.gps_baud = result.gps_baud * 10 + static_cast<unsigned>(digit - '0');
    }
    if (version != "1") integer("tuning_offset_hz", result.tuning_offset_hz);
    if (version == "5" || version == "6") {
        const auto& source = fields.at("receiver_source");
        if (source != "synthetic" && source != "hackrf" && !(version == "6" && source == "rtl_sdr"))
            throw std::runtime_error("Unsupported preferred receiver");
        result.receiver_source = source == "hackrf" ? DesktopReceiver::HackRf :
            source == "rtl_sdr" ? DesktopReceiver::RtlSdr : DesktopReceiver::Synthetic;
        integer("center_hz", result.center_hz);
        integer("sample_rate", result.sample_rate);
        integer("survey_span_hz", result.survey_span_hz);
        integer("lna_gain", result.lna_gain);
        integer("vga_gain", result.vga_gain);
        result.amplifier = boolean("amplifier");
        result.decode_enabled = boolean("decode_enabled");
        result.spectrum_only = boolean("spectrum_only");
        result.mixed_fonts = boolean("mixed_fonts");
        result.mobile_position_display = boolean("mobile_position_display");
    }
    if (version == "6") {
        integer("rtl_gain_tenths_db", result.rtl_gain_tenths_db);
        result.rtl_auto_gain = boolean("rtl_auto_gain");
    }
    validate_preferences(result);
    return result;
}

std::string serialize(const DesktopPreferences& preferences) {
    validate_preferences(preferences);
    return std::string("version=6\ngps_enabled=") + (preferences.gps_enabled ? "1" : "0") +
        "\nrecording_enabled=" + (preferences.recording_enabled ? "1" : "0") +
        "\nrecording_directory=" + hex_encode(preferences.recording_directory) +
        "\ngps_device_id=" + hex_encode(preferences.gps_device_id) +
        "\ngps_baud=" + std::to_string(preferences.gps_baud) +
        "\ntuning_offset_hz=" + std::to_string(preferences.tuning_offset_hz) +
        "\ndiscover_lora=" + (preferences.discover_lora ? "1" : "0") +
        "\ncompact_recording=" + (preferences.compact_recording ? "1" : "0") +
        "\ndecode_enabled=" + (preferences.decode_enabled ? "1" : "0") +
        "\nspectrum_only=" + (preferences.spectrum_only ? "1" : "0") +
        "\nreceiver_source=" + (preferences.receiver_source == DesktopReceiver::HackRf ? "hackrf" :
            preferences.receiver_source == DesktopReceiver::RtlSdr ? "rtl_sdr" : "synthetic") +
        "\ncenter_hz=" + std::to_string(preferences.center_hz) +
        "\nsample_rate=" + std::to_string(preferences.sample_rate) +
        "\nsurvey_span_hz=" + std::to_string(preferences.survey_span_hz) +
        "\nlna_gain=" + std::to_string(preferences.lna_gain) +
        "\nvga_gain=" + std::to_string(preferences.vga_gain) +
        "\namplifier=" + (preferences.amplifier ? "1" : "0") +
        "\nmixed_fonts=" + (preferences.mixed_fonts ? "1" : "0") +
        "\nmobile_position_display=" + (preferences.mobile_position_display ? "1" : "0") +
        "\nrtl_gain_tenths_db=" + std::to_string(preferences.rtl_gain_tenths_db) +
        "\nrtl_auto_gain=" + (preferences.rtl_auto_gain ? "1" : "0") + "\n";
}

bool read_settings(const PreferencePaths& paths, std::string& text) {
    if (!directory_exists(path_from(paths.directory))) return false;
    verify_private_directory(path_from(paths.directory));
    validate_local_file_path(paths.settings_file);
    const auto file = path_from(paths.settings_file);
    std::error_code error;
    const auto status = fs::symlink_status(file, error);
    if (missing(error, status)) return false;
    if (error || !fs::is_regular_file(status)) throw std::runtime_error("Desktop settings are not a regular local file");
#ifdef _WIN32
    Handle handle(CreateFileW(file.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot read desktop settings");
    verify_private_handle(handle.get(), false);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.get(), &size) || size.QuadPart < 0 || size.QuadPart > static_cast<LONGLONG>(maximum_settings_size))
        throw std::runtime_error("Oversized desktop settings");
#else
    Descriptor handle(::open(file.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (handle.get() < 0) throw std::runtime_error("Cannot read desktop settings");
    struct stat info{};
    if (::fstat(handle.get(), &info) != 0) throw std::runtime_error("Cannot inspect desktop settings");
    verify_private_stat(info, false);
    if (info.st_size < 0 || static_cast<uintmax_t>(info.st_size) > maximum_settings_size)
        throw std::runtime_error("Oversized desktop settings");
#endif
    std::array<char, maximum_settings_size + 1> buffer{};
    size_t length = 0;
    while (length < buffer.size()) {
#ifdef _WIN32
        DWORD count = 0;
        if (!ReadFile(handle.get(), buffer.data() + length, static_cast<DWORD>(buffer.size() - length), &count, nullptr))
            throw std::runtime_error("Cannot read desktop settings");
#else
        const auto count = ::read(handle.get(), buffer.data() + length, buffer.size() - length);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::runtime_error("Cannot read desktop settings");
#endif
        if (count == 0) break;
        length += static_cast<size_t>(count);
    }
    if (length > maximum_settings_size) throw std::runtime_error("Oversized desktop settings");
    text.assign(buffer.data(), length);
    return true;
}

std::string random_suffix() {
    std::array<unsigned char, 12> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) throw std::runtime_error("Cannot generate a unique local filename");
    return hex_encode(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

void atomic_write(const PreferencePaths& paths, const std::string& text) {
    const auto directory = path_from(paths.directory);
    const auto temporary = directory / (".preferences-" + random_suffix() + ".tmp");
    validate_local_file_path(utf8(temporary));
    bool created = false, replaced = false;
    struct RemoveTemporary {
        const fs::path& file; bool& created; bool& replaced;
        ~RemoveTemporary() { if (created && !replaced) { std::error_code error; fs::remove(file, error); } }
    } cleanup{temporary, created, replaced};
    {
#ifdef _WIN32
        PrivateSecurity security;
        Handle handle(CreateFileW(temporary.c_str(), GENERIC_WRITE | READ_CONTROL, 0, security.attributes(),
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create private settings file");
        created = true; verify_private_handle(handle.get(), false);
#else
        Descriptor handle(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (handle.get() < 0) throw std::runtime_error("Cannot create private settings file");
        created = true;
#endif
        size_t offset = 0;
        while (offset < text.size()) {
#ifdef _WIN32
            DWORD count = 0;
            if (!WriteFile(handle.get(), text.data() + offset, static_cast<DWORD>(text.size() - offset), &count, nullptr) || count == 0)
                throw std::runtime_error("Cannot write desktop settings");
#else
            const auto count = ::write(handle.get(), text.data() + offset, text.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("Cannot write desktop settings");
#endif
            offset += static_cast<size_t>(count);
        }
#ifdef _WIN32
        if (!FlushFileBuffers(handle.get())) throw std::runtime_error("Cannot flush desktop settings");
#else
        if (::fsync(handle.get()) != 0) throw std::runtime_error("Cannot flush desktop settings");
#endif
    }
    // Recheck the destination after preparing the replacement. A broken or
    // unrecognized configuration is left intact rather than silently replaced.
    std::string existing;
    if (read_settings(paths, existing)) (void)parse(existing);
    validate_local_file_path(paths.settings_file);
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path_from(paths.settings_file).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace desktop settings");
#else
    if (::rename(temporary.c_str(), path_from(paths.settings_file).c_str()) != 0)
        throw std::runtime_error("Cannot replace desktop settings");
#endif
    replaced = true;
#ifndef _WIN32
    Descriptor parent(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (parent.get() < 0 || ::fsync(parent.get()) != 0)
        throw std::runtime_error("Settings were saved, but their directory could not be flushed");
#endif
}

} // namespace

PreferencePaths preference_paths(const std::string& explicit_directory) {
    fs::path directory;
    if (!explicit_directory.empty()) directory = absolute_directory(explicit_directory);
    else {
#ifdef _WIN32
        const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (count == 0 || count > maximum_path_size) throw std::runtime_error("Local application-data directory is unavailable");
        std::wstring value(count, L'\0');
        const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), count);
        if (copied == 0 || copied >= count) throw std::runtime_error("Local application-data directory is unavailable");
        value.resize(copied);
        directory = fs::path(value) / "OVMeshDRpp";
#else
        const char* home = std::getenv("HOME");
#if defined(__APPLE__)
        if (!home || !*home) throw std::runtime_error("Home directory is unavailable");
        directory = path_from(home) / "Library" / "Application Support" / "OVMeshDRpp";
#else
        const char* state = std::getenv("XDG_STATE_HOME");
        if (state && *state) directory = path_from(state) / "ovmeshdrpp";
        else {
            if (!home || !*home) throw std::runtime_error("Home directory is unavailable");
            directory = path_from(home) / ".local" / "state" / "ovmeshdrpp";
        }
#endif
#endif
        directory = absolute_directory(utf8(directory));
    }
    return {utf8(directory), utf8(directory / "preferences.conf"), utf8(directory / "Surveys")};
}

void ensure_preferences_directories(const PreferencePaths& paths) {
    check_paths(paths);
    create_directory_tree(path_from(paths.directory));
    create_directory_tree(path_from(paths.surveys_directory));
}

DesktopPreferences load_preferences(const PreferencePaths& paths) {
    check_paths(paths);
    DesktopPreferences defaults; defaults.recording_directory = paths.surveys_directory;
    std::string text;
    return read_settings(paths, text) ? parse(text) : defaults;
}

void save_preferences(const PreferencePaths& paths, const DesktopPreferences& preferences) {
    check_paths(paths);
    const auto text = serialize(preferences);
    std::string existing;
    if (read_settings(paths, existing)) (void)parse(existing);
    ensure_preferences_directories(paths);
    atomic_write(paths, text);
}

std::string new_survey_path(const std::string& directory) {
    const auto folder = absolute_directory(directory);
    validate_local_file_path(utf8(folder / ".ovmesh-survey-path-check"));
    if (!directory_exists(folder)) throw std::runtime_error("Recording directory is unavailable; choose another local folder");
    const auto now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    if (::gmtime_s(&utc, &now) != 0) throw std::runtime_error("Cannot determine the survey start time");
#else
    if (::gmtime_r(&now, &utc) == nullptr) throw std::runtime_error("Cannot determine the survey start time");
#endif
    std::array<char, 32> date{};
    if (std::strftime(date.data(), date.size(), "%Y%m%d-%H%M%SZ", &utc) == 0) throw std::runtime_error("Cannot format the survey filename");
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const auto file = folder / (std::string("survey-") + date.data() + "-" + random_suffix() + ".sqlite");
        validate_local_file_path(utf8(file));
        std::error_code error;
        const auto status = fs::symlink_status(file, error);
        if (missing(error, status)) return utf8(file);
        if (error) throw std::runtime_error("Cannot inspect the new survey destination");
    }
    throw std::runtime_error("Cannot choose a new survey filename");
}

} // namespace ovmesh
