// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/preferences.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
namespace fs = std::filesystem;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F operation, const char* message) {
    try { operation(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
std::string utf8(const fs::path& path) { const auto value = path.u8string(); return {value.begin(), value.end()}; }
struct Fixture {
    fs::path folder;
    Fixture() {
        // CTest's working folder is inside the repository build. Every settings
        // API receives an explicit override; never write the actual user profile.
        folder = fs::current_path() / ("preferences-fixture-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(fs::create_directory(folder), "Create fixture directory");
    }
    ~Fixture() { std::error_code error; fs::remove_all(folder, error); }
    ovmesh::PreferencePaths paths(const std::string& name) const { return ovmesh::preference_paths(utf8(folder / name)); }
};
std::string contents(const std::string& filename) {
    std::ifstream stream(fs::path(std::u8string(filename.begin(), filename.end())), std::ios::binary);
    require(stream.good(), "Read fixture settings");
    return {std::istreambuf_iterator<char>(stream), {}};
}
void write_existing(const std::string& filename, const std::string& text) {
    std::ofstream stream(fs::path(std::u8string(filename.begin(), filename.end())), std::ios::binary | std::ios::trunc);
    stream << text; stream.close(); require(stream.good(), "Write deliberately malformed fixture settings");
}
void replace(std::string& text, const std::string& before, const std::string& after) {
    const auto where = text.find(before); require(where != std::string::npos, "Find fixture field");
    text.replace(where, before.size(), after);
}

void defaults_and_roundtrip(const Fixture& fixture) {
    const auto paths = fixture.paths("new-profile");
    const auto defaults = ovmesh::load_preferences(paths);
    require(defaults.gps_enabled && defaults.recording_enabled && defaults.compact_recording && defaults.gps_baud == 9600 && defaults.tuning_offset_hz == 0 && defaults.discover_lora && defaults.decode_enabled && !defaults.spectrum_only,
        "First-run recording/GPS/discovery enabled; receiver offset zero and authorized decoding armed");
    require(defaults.receiver_source == ovmesh::DesktopReceiver::HackRf && defaults.center_hz == 907500000 &&
        defaults.sample_rate == 16000000 && defaults.survey_span_hz == 10000000 && defaults.lna_gain == 16 &&
        defaults.vga_gain == 16 && !defaults.amplifier && defaults.mixed_fonts && !defaults.mobile_position_display,
        "First-run receiver metadata, mixed typography, and stationary display defaults");
    require(defaults.gps_device_id.empty() && defaults.recording_directory == paths.surveys_directory,
        "First-run default survey folder and automatic GPS selection");
    require(!fs::exists(fs::path(paths.directory)), "Loading missing settings creates nothing");
    ovmesh::ensure_preferences_directories(paths);
    require(fs::is_directory(fs::path(paths.surveys_directory)), "Explicit initialization creates default survey folder");
    require(!fs::exists(fs::path(paths.settings_file)), "Directory initialization does not create settings");
    ovmesh::save_preferences(paths, defaults);
    require(ovmesh::load_preferences(paths) == defaults, "Default settings round trip");
    require(contents(paths.settings_file).starts_with("version=7\n"), "New preferences use version 7");

    auto changed = defaults;
    const auto alternate = fixture.folder / fs::path(u8"région-測定"); fs::create_directory(alternate);
    changed.recording_directory = utf8(alternate);
    changed.gps_device_id = "usb:1234:5678:s\xc3\xa9rie-##%=safe";
    changed.gps_enabled = false; changed.recording_enabled = false; changed.gps_baud = 115200;
    changed.discover_lora = false; changed.decode_enabled = false; changed.compact_recording = false; changed.spectrum_only = true;
    changed.receiver_source = ovmesh::DesktopReceiver::Synthetic;
    changed.center_hz = 433775000; changed.sample_rate = 8000000; changed.survey_span_hz = 5000000;
    changed.lna_gain = 40; changed.vga_gain = 62; changed.amplifier = true;
    changed.mixed_fonts = false; changed.mobile_position_display = true;
    ovmesh::save_preferences(paths, changed);
    require(ovmesh::load_preferences(paths) == changed, "Unicode path, GPS identity, disabled defaults and baud survive replacement");
    const auto text = contents(paths.settings_file);
    require(text.size() < 32768 && text.find(changed.gps_device_id) == std::string::npos,
        "Settings strings use bounded encoding");
    require(text.find("key") == std::string::npos && text.find("latitude") == std::string::npos &&
        text.find("longitude") == std::string::npos && text.find("payload") == std::string::npos,
        "Preference schema does not contain traffic, keys, or coordinates");
    auto long_identity = changed; long_identity.gps_device_id = std::string(4096, 'a');
    ovmesh::save_preferences(paths, long_identity);
    require(ovmesh::load_preferences(paths) == long_identity, "Full stable-ID limit round-trips without truncation");
    for (const auto& entry : fs::directory_iterator(fs::path(paths.directory)))
        require(entry.path().extension() != ".tmp", "Atomic save leaves no temporary file");
#ifndef _WIN32
    struct stat status{};
    require(::stat(paths.directory.c_str(), &status) == 0 && (status.st_mode & 0777) == 0700, "Settings directory permissions");
    require(::stat(paths.surveys_directory.c_str(), &status) == 0 && (status.st_mode & 0777) == 0700, "Default survey directory permissions");
    require(::stat(paths.settings_file.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600, "Settings file permissions");
#endif
}

void tuning_offset_roundtrip_and_migration(const Fixture& fixture) {
    const auto paths = fixture.paths("tuning-offset");
    auto preferences = ovmesh::load_preferences(paths);
    preferences.gps_enabled = false; preferences.recording_enabled = false;
    preferences.gps_device_id = "remembered-serial-gps"; preferences.gps_baud = 57600;
    const auto alternate = fixture.folder / "remembered-surveys"; fs::create_directory(alternate);
    preferences.recording_directory = utf8(alternate);
    for (const int64_t offset : {int64_t{900}, int64_t{-900}, int64_t{100000}, int64_t{-100000}, int64_t{0}}) {
        preferences.tuning_offset_hz = offset;
        ovmesh::save_preferences(paths, preferences);
        require(ovmesh::load_preferences(paths) == preferences, "Signed tuning offset round trips without changing other preferences");
    }
    const auto version7 = contents(paths.settings_file);
    auto version6 = version7;
    replace(version6, "version=7\n", "version=6\n");
    for (size_t at = version6.find("rak_"); at != std::string::npos; at = version6.find("rak_"))
        version6.erase(at, version6.find('\n', at) - at + 1);
    write_existing(paths.settings_file, version6);
    require(ovmesh::load_preferences(paths) == preferences, "Version 6 preserves SDR preferences and adopts unselected RAK defaults");
    auto version5 = version6;
    replace(version5, "version=6\n", "version=5\n");
    replace(version5, "rtl_gain_tenths_db=280\n", "");
    replace(version5, "rtl_auto_gain=0\n", "");
    write_existing(paths.settings_file, version5);
    require(ovmesh::load_preferences(paths) == preferences,
        "Version 5 adopts absent RTL defaults without changing the saved receiver");
    require(contents(paths.settings_file) == version5, "Loading version 5 leaves its file unchanged");
    auto version4 = version5;
    replace(version4, "version=5\n", "version=4\n");
    for (const auto* line : {"spectrum_only=0\n", "decode_enabled=1\n", "receiver_source=hackrf\n", "center_hz=907500000\n",
            "sample_rate=16000000\n", "survey_span_hz=10000000\n", "lna_gain=16\n", "vga_gain=16\n",
            "amplifier=0\n", "mixed_fonts=1\n", "mobile_position_display=0\n"})
        replace(version4, line, "");
    // Earlier discovery opt-outs are explicit and must survive the new default.
    replace(version4, "discover_lora=1\n", "discover_lora=0\n");
    auto discovery_opt_out = preferences; discovery_opt_out.discover_lora = false;
    write_existing(paths.settings_file, version4);
    require(ovmesh::load_preferences(paths) == discovery_opt_out,
        "Version 4 preserves explicit discovery opt-out and adopts absent receiver/display defaults");
    require(contents(paths.settings_file) == version4, "Loading version 4 leaves its file unchanged");
    auto version3 = version4;
    replace(version3, "version=4\n", "version=3\n");
    replace(version3, "compact_recording=1\n", "");
    write_existing(paths.settings_file, version3);
    require(ovmesh::load_preferences(paths) == discovery_opt_out && ovmesh::load_preferences(paths).compact_recording,
        "Version 3 preserves discovery opt-out and defaults new recordings to compact");
    require(contents(paths.settings_file) == version3, "Loading version 3 leaves its file unchanged");
    auto version2 = version3;
    replace(version2, "version=3\n", "version=2\n");
    replace(version2, "discover_lora=0\n", "");
    write_existing(paths.settings_file, version2);
    require(ovmesh::load_preferences(paths) == preferences && ovmesh::load_preferences(paths).discover_lora,
        "Version 2 preserves every field and adopts discovery enabled when no prior choice exists");
    require(contents(paths.settings_file) == version2, "Loading version 2 leaves its file unchanged");
    auto legacy = version2;
    replace(legacy, "version=2\n", "version=1\n");
    replace(legacy, "tuning_offset_hz=0\n", "");
    write_existing(paths.settings_file, legacy);
    const auto loaded_legacy = ovmesh::load_preferences(paths);
    require(loaded_legacy == preferences && loaded_legacy.tuning_offset_hz == 0,
        "Version 1 loads all retained settings and defaults tuning offset to zero");
    require(contents(paths.settings_file) == legacy, "Loading version 1 does not migrate or rewrite its file");
    ovmesh::save_preferences(paths, loaded_legacy);
    require(contents(paths.settings_file) == version7 && ovmesh::load_preferences(paths) == preferences,
        "Explicit save migrates version 1 to version 7 and retains every prior field");
    write_existing(paths.settings_file, version4);
    ovmesh::save_preferences(paths, ovmesh::load_preferences(paths));
    require(ovmesh::load_preferences(paths) == discovery_opt_out && contents(paths.settings_file).starts_with("version=7\n"),
        "Explicit migration to version 7 preserves an older discovery opt-out");
}

void new_files_do_not_overwrite(const Fixture& fixture) {
    const auto paths = fixture.paths("unique-surveys"); ovmesh::ensure_preferences_directories(paths);
    std::set<std::string> names;
    for (unsigned i = 0; i < 100; ++i) {
        const auto filename = ovmesh::new_survey_path(paths.surveys_directory);
        require(names.insert(filename).second, "Survey filenames are unique even within the same second");
        const auto path = fs::path(std::u8string(filename.begin(), filename.end()));
        require(path.parent_path() == fs::path(paths.surveys_directory) && path.extension() == ".sqlite" &&
            utf8(path.filename()).starts_with("survey-"), "Survey filename uses selected folder, format and prefix");
        require(!fs::exists(path), "Selecting a new survey path never creates a file");
    }
    require(fs::is_empty(fs::path(paths.surveys_directory)), "Filename generation does not leave artifacts");
    const auto absent = utf8(fixture.folder / "unselected-missing-folder");
    rejects([&] { ovmesh::new_survey_path(absent); }, "Missing selected recording directory must fail");
    require(!fs::exists(fs::path(absent)), "No arbitrary recording directory creation");
}

void malformed_settings_preserved(const Fixture& fixture) {
    const auto paths = fixture.paths("malformed");
    const auto defaults = ovmesh::load_preferences(paths); ovmesh::save_preferences(paths, defaults);
    const auto valid = contents(paths.settings_file);
    std::set<std::string> malformed = {"", valid.substr(0, valid.size() - 1), std::string(32769, 'x'), valid + "unexpected=1\n"};
    for (const auto& [from, to] : {
            std::pair<std::string, std::string>{"version=7", "version=8"},
            {"gps_enabled=1", "gps_enabled=true"}, {"recording_enabled=1", "recording_enabled=2"},
            {"gps_baud=9600", "gps_baud=0"}, {"gps_baud=9600", "gps_baud=230400"},
            {"gps_baud=9600", "gps_baud=09600"}, {"gps_baud=9600", "gps_baud=9999999"},
            {"gps_device_id=", "gps_device_id=00"}, {"gps_device_id=", "gps_device_id=zz"},
            {"gps_device_id=", "gps_device_id=0"}, {"gps_device_id=", "gps_device_id=c0af"},
            {"gps_device_id=", "gps_device_id=edb080"}, {"gps_device_id=", "gps_device_id=f4908080"},
            {"gps_device_id=", "gps_enabled=1"}, {"gps_device_id=", "unknown="},
            {"version=7", "version=1"}, {"version=7", "version=2"}, {"version=7", "version=3"},
            {"version=7", "version=4"}, {"version=7", "version=5"}, {"tuning_offset_hz=0\n", ""},
            {"discover_lora=1", "discover_lora=true"}, {"discover_lora=1", "discover_lora=2"},
            {"discover_lora=1\n", ""}, {"discover_lora=1", "gps_enabled=1"},
            {"compact_recording=1\n", ""}, {"compact_recording=1", "compact_recording=2"},
            {"compact_recording=1", "compact_recording=true"}, {"compact_recording=1", "recording_enabled=1"},
            {"tuning_offset_hz=0", "gps_baud=9600"}, {"tuning_offset_hz=0", "unknown=0"},
            {"receiver_source=hackrf", "receiver_source=HackRF"}, {"receiver_source=hackrf", "receiver_source=2"},
            {"receiver_source=hackrf", "receiver_source="}, {"receiver_source=hackrf\n", ""},
            {"center_hz=907500000", "center_hz=999999"}, {"center_hz=907500000", "center_hz=6000000001"},
            {"center_hz=907500000", "center_hz=1000000"}, {"center_hz=907500000", "center_hz=5999999999"},
            {"sample_rate=16000000", "sample_rate=16000001"}, {"sample_rate=16000000", "sample_rate=8000000"},
            {"survey_span_hz=10000000", "survey_span_hz=499999"}, {"survey_span_hz=10000000", "survey_span_hz=12800001"},
            {"lna_gain=16", "lna_gain=7"}, {"lna_gain=16", "lna_gain=48"},
            {"vga_gain=16", "vga_gain=3"}, {"vga_gain=16", "vga_gain=64"},
            {"rtl_gain_tenths_db=280", "rtl_gain_tenths_db=601"}, {"rtl_gain_tenths_db=280", "rtl_gain_tenths_db=-101"},
            {"rtl_auto_gain=0", "rtl_auto_gain=2"}, {"amplifier=0", "amplifier=true"}, {"spectrum_only=0", "spectrum_only=2"}, {"decode_enabled=1", "decode_enabled=true"},
            {"mixed_fonts=1", "mixed_fonts=0 "}, {"mobile_position_display=0", "mobile_position_display=-1"}}) {
        auto text = valid; replace(text, from, to); malformed.insert(text);
    }
    for (const auto* field : {"center_hz", "sample_rate", "survey_span_hz", "lna_gain", "vga_gain",
            "amplifier", "spectrum_only", "decode_enabled", "mixed_fonts", "mobile_position_display",
            "rtl_gain_tenths_db", "rtl_auto_gain"}) {
        const auto start = valid.find(std::string(field) + '=');
        const auto line = valid.substr(start, valid.find('\n', start) + 1 - start);
        auto missing = valid; replace(missing, line, ""); malformed.insert(missing);
        auto unknown = valid; replace(unknown, line, "unknown=0\n"); malformed.insert(unknown);
        auto duplicate = valid; replace(duplicate, line, "gps_enabled=1\n"); malformed.insert(duplicate);
    }
    for (const auto* field : {"center_hz", "sample_rate", "survey_span_hz", "lna_gain", "vga_gain"}) {
        const auto start = valid.find(std::string(field) + '=');
        const auto line = valid.substr(start, valid.find('\n', start) + 1 - start);
        for (const auto* value : {"", "-1", "+16", "016", "16.0", "1e6", "NaN", " 16", "16 ",
                "18446744073709551616", "999999999999999999999999999999999999"}) {
            auto malformed_number = valid;
            replace(malformed_number, line, std::string(field) + '=' + value + '\n');
            malformed.insert(malformed_number);
        }
    }
    for (const auto* offset : {"", "100001", "-100001", "9223372036854775808", "-9223372036854775809",
            "999999999999999999999999999999999999999999", "+900", "0900", "-0900", "00", "-0", "900.0",
            "9e2", "900Hz", " 900", "900 ", "900\r", "--900", "NaN", "-", "+"}) {
        auto text = valid; replace(text, "tuning_offset_hz=0", std::string("tuning_offset_hz=") + offset); malformed.insert(text);
    }
    for (const auto& text : malformed) {
        write_existing(paths.settings_file, text);
        rejects([&] { ovmesh::load_preferences(paths); }, "Malformed settings must fail on load");
        rejects([&] { ovmesh::save_preferences(paths, defaults); }, "Malformed settings must not be silently replaced");
        require(contents(paths.settings_file) == text, "Malformed settings preserved verbatim");
    }
    write_existing(paths.settings_file, valid);
    for (const unsigned baud : {0U, 1U, 230400U, 4294967295U}) {
        auto invalid = defaults; invalid.gps_baud = baud;
        rejects([&] { ovmesh::save_preferences(paths, invalid); }, "Unsupported baud must not be persisted");
    }
    for (const int64_t offset : {int64_t{100001}, int64_t{-100001}, std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min()}) {
        auto invalid = defaults; invalid.tuning_offset_hz = offset;
        rejects([&] { ovmesh::save_preferences(paths, invalid); }, "Out-of-range tuning offset must not be persisted");
    }
    auto invalid = defaults; invalid.gps_device_id = std::string(4097, 'a');
    rejects([&] { ovmesh::save_preferences(paths, invalid); }, "Oversized GPS identity rejected");
    invalid = defaults; invalid.recording_directory = "relative-folder";
    rejects([&] { ovmesh::save_preferences(paths, invalid); }, "Relative recording preference rejected");
    require(contents(paths.settings_file) == valid, "Rejected writes preserve valid settings");
}

void receiver_bounds(const Fixture& fixture) {
    const auto paths = fixture.paths("receiver-bounds");
    auto preferences = ovmesh::load_preferences(paths);
    for (const uint32_t rate : {8000000U, 10000000U, 12000000U, 16000000U, 20000000U}) {
        preferences.sample_rate = rate; preferences.survey_span_hz = rate * 4 / 5;
        for (const unsigned lna : {0U, 8U, 16U, 24U, 32U, 40U}) {
            preferences.lna_gain = lna; preferences.vga_gain = lna;
            ovmesh::save_preferences(paths, preferences);
            require(ovmesh::load_preferences(paths) == preferences, "Valid RF sample rates, spans, and gain steps round trip");
        }
    }
    preferences.survey_span_hz = 500000; preferences.lna_gain = 0; preferences.vga_gain = 62;
    for (const uint64_t center : {1000000ULL, 5999750000ULL}) {
        preferences.center_hz = center;
        ovmesh::save_preferences(paths, preferences);
        require(ovmesh::load_preferences(paths) == preferences, "RF center and survey edge boundaries round trip");
    }
    const auto valid = contents(paths.settings_file);
    for (const auto mutate : {
            +[](ovmesh::DesktopPreferences& p) { p.receiver_source = static_cast<ovmesh::DesktopReceiver>(4); },
            +[](ovmesh::DesktopPreferences& p) { p.center_hz = std::numeric_limits<uint64_t>::max(); },
            +[](ovmesh::DesktopPreferences& p) { p.center_hz = 1000000; p.tuning_offset_hz = -1; },
            +[](ovmesh::DesktopPreferences& p) { p.center_hz = 5999750001ULL; },
            +[](ovmesh::DesktopPreferences& p) { p.sample_rate = std::numeric_limits<uint32_t>::max(); },
            +[](ovmesh::DesktopPreferences& p) { p.survey_span_hz = 499999; },
            +[](ovmesh::DesktopPreferences& p) { p.survey_span_hz = std::numeric_limits<uint32_t>::max(); },
            +[](ovmesh::DesktopPreferences& p) { p.lna_gain = 1; },
            +[](ovmesh::DesktopPreferences& p) { p.vga_gain = 63; }}) {
        auto invalid = preferences; mutate(invalid);
        rejects([&] { ovmesh::save_preferences(paths, invalid); }, "Out-of-range RF preference cannot be persisted");
        require(contents(paths.settings_file) == valid, "Invalid RF preference preserves previous valid settings");
    }
}

void rtl_receiver_roundtrip(const Fixture& fixture) {
    const auto paths = fixture.paths("rtl-profile");
    auto preferences = ovmesh::load_preferences(paths);
    preferences.receiver_source = ovmesh::DesktopReceiver::RtlSdr;
    preferences.center_hz = 906875000;
    preferences.tuning_offset_hz = -600;
    preferences.sample_rate = 2000000;
    preferences.survey_span_hz = 1500000;
    for (const int gain : {-100, 0, 280, 496, 600}) for (const bool automatic : {false, true}) {
        preferences.rtl_gain_tenths_db = gain; preferences.rtl_auto_gain = automatic;
        ovmesh::save_preferences(paths, preferences);
        require(ovmesh::load_preferences(paths) == preferences,
            "RTL source, signed Offset, gain and automatic/manual choice survive restart");
    }
    preferences.discover_lora = false; preferences.sample_rate = 1000000; preferences.survey_span_hz = 800000;
    ovmesh::save_preferences(paths, preferences);
    require(ovmesh::load_preferences(paths) == preferences, "RTL 1 MS/s survey retains its narrower range");
    const auto valid = contents(paths.settings_file);
    for (const auto mutate : {
            +[](ovmesh::DesktopPreferences& p) { p.sample_rate = 16000000; },
            +[](ovmesh::DesktopPreferences& p) { p.sample_rate = 2400000; },
            +[](ovmesh::DesktopPreferences& p) { p.survey_span_hz = 800001; },
            +[](ovmesh::DesktopPreferences& p) { p.amplifier = true; },
            +[](ovmesh::DesktopPreferences& p) { p.rtl_gain_tenths_db = -101; },
            +[](ovmesh::DesktopPreferences& p) { p.rtl_gain_tenths_db = 601; }}) {
        auto invalid = preferences; mutate(invalid);
        rejects([&] { ovmesh::save_preferences(paths, invalid); }, "Unsupported RTL preference is rejected before replacing valid settings");
        require(contents(paths.settings_file) == valid, "Rejected RTL changes preserve the recorded preference file");
    }
}

void concentrator_roundtrip(const Fixture& fixture) {
    const auto paths = fixture.paths("concentrator-profile");
    auto preferences = ovmesh::load_preferences(paths);
    preferences.receiver_source = ovmesh::DesktopReceiver::Rak5146;
    preferences.center_hz = 915000000; preferences.survey_span_hz = 26000000;
    require(preferences.concentrators.boards.size() == 1 && preferences.concentrators.boards[0].device_path.empty() &&
        preferences.concentrators.boards[0].device_id.empty(), "Shipped concentrator preferences contain no device identity or path");
    ovmesh::save_preferences(paths, preferences);
    require(ovmesh::load_preferences(paths) == preferences, "Unselected RAK setup can be remembered without opening hardware");
    auto& boards = preferences.concentrators.boards;
    boards[0].device_path = "/dev/ttyACM90"; boards[0].device_id = "usb:0483:5740:serial:544553544f4e45";
    boards.push_back(ovmesh::ConcentratorBoardConfig{});
    boards[1].device_path = "COM91"; boards[1].device_id = "usb:0483:5740:serial:5445535454574f";
    boards[1].frequency_hz = 908750000; boards[1].bandwidth_hz = 500000;
    preferences.concentrators.scan_step_hz = 100000;
    preferences.concentrators.decode_enabled = false;
    preferences.decode_enabled = false;
    ovmesh::save_preferences(paths, preferences);
    require(ovmesh::load_preferences(paths) == preferences, "Two independent board identities and packet/scan settings round trip");
    const auto valid = contents(paths.settings_file);
    require(valid.find(boards[0].device_path) == std::string::npos && valid.find(boards[1].device_id) == std::string::npos,
        "Private board metadata uses bounded preference text encoding");
    for (const auto mutate : {
            +[](ovmesh::DesktopPreferences& p) { p.concentrators.boards.clear(); },
            +[](ovmesh::DesktopPreferences& p) { p.concentrators.boards.resize(3); },
            +[](ovmesh::DesktopPreferences& p) { p.concentrators.boards[0].device_path = "//invalid.invalid/tty"; },
            +[](ovmesh::DesktopPreferences& p) { p.concentrators.boards[0].device_id = "usb:1546:01a7:serial:54455354"; },
            +[](ovmesh::DesktopPreferences& p) { p.concentrators.boards[0].device_id.clear(); },
            +[](ovmesh::DesktopPreferences& p) { p.concentrators.boards[0].sync_word = 255; },
            +[](ovmesh::DesktopPreferences& p) { p.concentrators.boards[0].spreading_factor = 13; },
            +[](ovmesh::DesktopPreferences& p) { p.concentrators.boards[0].bandwidth_hz = 62500; },
            +[](ovmesh::DesktopPreferences& p) { p.concentrators.scan_samples = 1; },
            +[](ovmesh::DesktopPreferences& p) { p.concentrators.scan_step_hz = 24999; },
            +[](ovmesh::DesktopPreferences& p) { p.survey_span_hz = 26000001; }}) {
        auto invalid = preferences; mutate(invalid);
        rejects([&] { ovmesh::save_preferences(paths, invalid); }, "Invalid concentrator preferences are rejected");
        require(contents(paths.settings_file) == valid, "Rejected concentrator settings preserve the previous profile");
    }
}

void unsafe_paths(const Fixture& fixture) {
    for (const auto& path : {std::string("relative"), std::string("//invalid.invalid/share"),
                           std::string("\\\\invalid.invalid\\share"), utf8(fixture.folder) + "/../escape"})
        rejects([&] { ovmesh::preference_paths(path); }, "Unsafe settings folder accepted");
    rejects([&] { ovmesh::preference_paths(std::string("/bad\0path", 9)); }, "NUL in settings path accepted");
#ifndef _WIN32
    const auto mounted = ovmesh::preference_paths("/Volumes/unapproved-test/OVMeshDRpp");
    rejects([&] { ovmesh::load_preferences(mounted); }, "Mounted volumes must be rejected before traversal");
    rejects([&] { ovmesh::ensure_preferences_directories(mounted); }, "Mounted volumes must be rejected before creation");
#endif
    auto forged = fixture.paths("forged"); forged.settings_file = utf8(fixture.folder / "other.conf");
    rejects([&] { ovmesh::load_preferences(forged); }, "Settings file must stay in its app directory");
    const auto paths = fixture.paths("aliases"); ovmesh::ensure_preferences_directories(paths);
    const auto defaults = ovmesh::load_preferences(paths); ovmesh::save_preferences(paths, defaults);
    const auto real_file = fs::path(paths.settings_file);
    const auto preserved = contents(paths.settings_file);
    const auto link = fixture.folder / "profile-alias";
    std::error_code error;
    fs::create_directory_symlink(fs::path(paths.directory), link, error);
#ifdef _WIN32
    if (error) return; // Symlink creation may require Windows developer mode.
#else
    require(!error, "Create controlled symlink fixture");
#endif
    const auto alias_paths = ovmesh::preference_paths(utf8(link));
    rejects([&] { ovmesh::load_preferences(alias_paths); }, "Do not follow a settings-directory symlink");
    rejects([&] { ovmesh::save_preferences(alias_paths, defaults); }, "Do not write through a settings-directory symlink");
    rejects([&] { ovmesh::new_survey_path(utf8(link)); }, "Do not choose recording files through aliases");
    const auto original = fs::path(paths.directory) / "preserved.conf"; fs::rename(real_file, original);
    fs::create_symlink(original, real_file);
    rejects([&] { ovmesh::load_preferences(paths); }, "Do not follow a settings-file symlink");
    rejects([&] { ovmesh::save_preferences(paths, defaults); }, "Do not replace a settings-file symlink");
    require(contents(utf8(original)) == preserved, "Symlink target remains unchanged");
    fs::remove(real_file);
    fs::create_hard_link(original, real_file);
    rejects([&] { ovmesh::load_preferences(paths); }, "Do not load multiply-linked settings");
    rejects([&] { ovmesh::save_preferences(paths, defaults); }, "Do not replace multiply-linked settings");
    fs::remove(real_file);
#ifndef _WIN32
    require(::mkfifo(real_file.c_str(), 0600) == 0, "Create controlled FIFO fixture");
    rejects([&] { ovmesh::load_preferences(paths); }, "Reject special settings files without blocking");
    fs::remove(real_file); fs::rename(original, real_file);
    require(::chmod(real_file.c_str(), 0644) == 0, "Set deliberately nonprivate fixture permissions");
    rejects([&] { ovmesh::load_preferences(paths); }, "Nonprivate settings rejected");
    rejects([&] { ovmesh::save_preferences(paths, defaults); }, "Do not quietly replace nonprivate settings");
    require(::chmod(real_file.c_str(), 0600) == 0, "Restore synthetic file permissions");
    require(::chmod(paths.directory.c_str(), 0755) == 0, "Set deliberately nonprivate directory permissions");
    rejects([&] { ovmesh::load_preferences(paths); }, "Nonprivate application directory rejected");
    require(::chmod(paths.directory.c_str(), 0700) == 0, "Restore synthetic directory permissions");
#endif
}
} // namespace

int main() {
    try {
        Fixture fixture;
        defaults_and_roundtrip(fixture);
        tuning_offset_roundtrip_and_migration(fixture);
        new_files_do_not_overwrite(fixture);
        malformed_settings_preserved(fixture);
        receiver_bounds(fixture);
        rtl_receiver_roundtrip(fixture);
        concentrator_roundtrip(fixture);
        unsafe_paths(fixture);
        std::cout << "Desktop preference checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Desktop preference checks failed: " << error.what() << '\n';
        return 1;
    }
}
