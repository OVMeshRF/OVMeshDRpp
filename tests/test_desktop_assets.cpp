// SPDX-License-Identifier: GPL-3.0-or-later
// Generated color patterns only: no screen, radio, GPS, or operational data.
#include "desktop_assets.hpp"
#include "ovmesh/local_paths.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
namespace fs = std::filesystem;
size_t checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F operation, const char* message) {
    ++checks;
    try { operation(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
std::string utf8(const fs::path& path) {
    const auto bytes = path.u8string(); return {bytes.begin(), bytes.end()};
}
struct Fixture {
    fs::path directory;
    Fixture() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = fs::current_path() / ("desktop-assets-fixture-" + std::to_string(nonce));
        require(fs::create_directory(directory), "Create exclusive test folder");
    }
    ~Fixture() { std::error_code error; fs::remove_all(directory, error); }
};
std::vector<uint8_t> read(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    require(input.good(), "Open generated PNG");
    return {std::istreambuf_iterator<char>(input), {}};
}
uint32_t integer(std::span<const uint8_t> bytes) {
    require(bytes.size() >= 4, "Read bounded big-endian integer");
    return static_cast<uint32_t>(bytes[0]) << 24 | static_cast<uint32_t>(bytes[1]) << 16 |
           static_cast<uint32_t>(bytes[2]) << 8 | bytes[3];
}

// Independent, bit-at-a-time CRC verifier (the writer uses a lookup table).
uint32_t crc32(std::span<const uint8_t> bytes) {
    uint32_t result = 0xffffffffU;
    for (const auto byte : bytes) {
        result ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const bool carry = (result & 1) != 0;
            result /= 2;
            if (carry) result ^= 0xedb88320U;
        }
    }
    return ~result;
}
struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;
    size_t deflate_blocks = 0;
};

// Read PNG chunks and independently decode the stored DEFLATE blocks. This
// verifies the actual file, orientation and checksums rather than writer state.
Image decode(const fs::path& path) {
    const auto png = read(path);
    constexpr std::array<uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    require(png.size() >= signature.size() && std::equal(signature.begin(), signature.end(), png.begin()), "PNG signature");
    Image image;
    std::vector<uint8_t> compressed;
    bool ihdr = false, idat = false, ended = false;
    for (size_t offset = 8; offset < png.size();) {
        require(png.size() - offset >= 12, "Complete PNG chunk");
        const auto length = integer(std::span(png).subspan(offset, 4));
        require(length <= png.size() - offset - 12, "Bounded PNG chunk length");
        const std::string type(png.begin() + static_cast<ptrdiff_t>(offset + 4),
                               png.begin() + static_cast<ptrdiff_t>(offset + 8));
        const auto data = std::span(png).subspan(offset + 8, length);
        const auto crc = integer(std::span(png).subspan(offset + 8 + length, 4));
        require(crc == crc32(std::span(png).subspan(offset + 4, length + 4)), "PNG chunk CRC");
        require(!ended, "No chunks after IEND");
        if (type == "IHDR") {
            require(!ihdr && !idat && offset == 8 && length == 13, "Unique leading IHDR");
            ihdr = true;
            image.width = integer(data.first(4)); image.height = integer(data.subspan(4, 4));
            require(data[8] == 8 && data[9] == 6 && data[10] == 0 && data[11] == 0 && data[12] == 0,
                    "Noninterlaced RGBA8 PNG format");
        } else if (type == "IDAT") {
            require(ihdr && !ended, "IDAT follows IHDR");
            idat = true;
            compressed.insert(compressed.end(), data.begin(), data.end());
        } else if (type == "IEND") {
            require(ihdr && idat && length == 0 && offset + 12 == png.size(), "Final empty IEND");
            ended = true;
        } else {
            require(false, "PNG must not contain metadata or other chunks");
        }
        offset += static_cast<size_t>(length) + 12;
    }
    require(ended, "IEND present");
    require(compressed.size() >= 11 && compressed[0] == 0x78 && compressed[1] == 0x01, "Zlib stored stream header");
    require((static_cast<unsigned>(compressed[0]) * 256 + compressed[1]) % 31 == 0, "Zlib FCHECK");
    std::vector<uint8_t> filtered;
    size_t offset = 2;
    bool final = false;
    while (!final) {
        require(compressed.size() - offset >= 9, "Complete stored block and Adler checksum");
        const auto flags = compressed[offset++];
        require(flags == 0 || flags == 1, "Byte-aligned stored DEFLATE block");
        final = flags == 1;
        const auto length = static_cast<unsigned>(compressed[offset]) + static_cast<unsigned>(compressed[offset + 1]) * 256;
        const auto inverse = static_cast<unsigned>(compressed[offset + 2]) + static_cast<unsigned>(compressed[offset + 3]) * 256;
        offset += 4;
        require((length ^ inverse) == 0xffff, "DEFLATE length complement");
        require(length > 0 && length <= compressed.size() - offset - 4, "Stored block payload bounds");
        filtered.insert(filtered.end(), compressed.begin() + static_cast<ptrdiff_t>(offset),
                         compressed.begin() + static_cast<ptrdiff_t>(offset + length));
        offset += length;
        ++image.deflate_blocks;
    }
    require(compressed.size() - offset == 4, "One final Adler checksum, no trailing bytes");
    uint64_t first = 1, second = 0;
    for (const auto byte : filtered) { first = (first + byte) % 65521; second = (second + first) % 65521; }
    require(integer(std::span(compressed).subspan(offset, 4)) == ((second << 16) | first), "Zlib Adler checksum");
    const size_t row_bytes = static_cast<size_t>(image.width) * 4;
    require(filtered.size() == static_cast<size_t>(image.height) * (row_bytes + 1), "Exact filtered pixel length");
    for (uint32_t row = 0; row < image.height; ++row) {
        const size_t start = static_cast<size_t>(row) * (row_bytes + 1);
        require(filtered[start] == 0, "Filter none for every row");
        image.pixels.insert(image.pixels.end(), filtered.begin() + static_cast<ptrdiff_t>(start + 1),
                            filtered.begin() + static_cast<ptrdiff_t>(start + row_bytes + 1));
    }
    return image;
}

std::vector<uint8_t> pattern(uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (size_t index = 0; index < pixels.size(); ++index)
        pixels[index] = static_cast<uint8_t>((index * 71 + index / 19) % 256);
    return pixels;
}

void roundtrips(const fs::path& directory) {
    for (const auto& [width, height] : {std::pair{1U, 1U}, {3U, 2U}, {64U, 255U}, {129U, 129U}, {8192U, 1U}}) {
        const auto pixels = pattern(width, height);
        const auto path = directory / (std::to_string(width) + "x" + std::to_string(height) + ".png");
        ovmesh::write_waveform_png(utf8(path), width, height, pixels);
        const auto image = decode(path);
        require(image.width == width && image.height == height && image.pixels == pixels, "Exact RGBA roundtrip");
        const uint64_t raw_size = static_cast<uint64_t>(height) * (1 + static_cast<uint64_t>(width) * 4);
        require(image.deflate_blocks == (raw_size + 65534) / 65535, "Correct final block across 64 KiB boundary");
#ifndef _WIN32
        struct stat status{};
        require(::stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600, "Private PNG file permissions");
#endif
        rejects([&] { ovmesh::write_waveform_png(utf8(path), width, height, pixels); }, "Never overwrite an existing capture");
        require(decode(path).pixels == pixels, "Existing capture remains unchanged after rejected overwrite");
    }
    const std::vector<uint8_t> top_down{255, 0, 0, 255, 0, 255, 0, 127, 0, 0, 255, 64, 255, 255, 255, 0};
    const std::vector<uint8_t> bottom_up(top_down.begin() + 8, top_down.end());
    auto gl_pixels = bottom_up; gl_pixels.insert(gl_pixels.end(), top_down.begin(), top_down.begin() + 8);
    const auto path = directory / fs::path(u8"waveform-é.PNG");
    ovmesh::write_waveform_png(utf8(path), 2, 2, gl_pixels, true);
    require(decode(path).pixels == top_down, "OpenGL bottom-up orientation, alpha and Unicode filename");
}

void invalid_inputs(const fs::path& directory) {
    const std::array<uint8_t, 4> pixels{1, 2, 3, 4};
    const auto path = utf8(directory / "invalid.png");
    for (const auto& [width, height] : {std::pair{0U, 1U}, {1U, 0U}, {8193U, 1U}, {1U, 8193U},
                                      {8192U, 8192U}, {std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max()}})
        rejects([&] { ovmesh::write_waveform_png(path, width, height, pixels); }, "Invalid dimensions rejected before output");
    rejects([&] { ovmesh::write_waveform_png(path, 1, 1, {}); }, "Missing RGBA bytes rejected");
    rejects([&] { ovmesh::write_waveform_png(path, 1, 1, std::span(pixels).first(3)); }, "Short RGBA buffer rejected");
    const std::array<uint8_t, 5> extra{};
    rejects([&] { ovmesh::write_waveform_png(path, 1, 1, extra); }, "Extra RGBA bytes rejected");
    require(!fs::exists(directory / "invalid.png"), "Invalid pixel input creates no output");
    for (const auto* name : {"wrong.csv", "extension-missing", "nul.png", "bad?.png", "bad\n.png", "COM1.png", "bad.png "})
        rejects([&] { ovmesh::write_waveform_png(utf8(directory / name), 1, 1, pixels); }, "Unsupported or unsafe filename rejected");
    for (const auto& name : {std::string("bad\0.png", 8), std::string("\xc0\xaf.png")})
        rejects([&] { ovmesh::write_waveform_png(utf8(directory) + "/" + name, 1, 1, pixels); }, "Invalid UTF-8 and NUL paths rejected");
    for (const auto* path_value : {"relative.png", "//invalid.invalid/share/capture.png", "\\\\invalid.invalid\\share\\capture.png", "/Volumes/unapproved/capture.png"})
        rejects([&] { ovmesh::write_waveform_png(path_value, 1, 1, pixels); }, "Nonlocal/relative path rejected without access");
    rejects([&] { ovmesh::write_waveform_png(utf8(directory) + "/../outside.png", 1, 1, pixels); }, "Dot-component traversal rejected");
    rejects([&] { ovmesh::write_waveform_png(utf8(directory / "missing" / "capture.png"), 1, 1, pixels); }, "No implicit directory creation");
    fs::create_directory(directory / "folder.png");
    rejects([&] { ovmesh::write_waveform_png(utf8(directory / "folder.png"), 1, 1, pixels); }, "Cannot replace a folder");
}

void aliases(const fs::path& directory) {
    const std::array<uint8_t, 4> pixels{1, 2, 3, 4};
    const auto real = directory / "real";
    fs::create_directory(real);
    std::error_code error;
    fs::create_directory_symlink(real, directory / "alias", error);
#ifdef _WIN32
    if (error) return; // Symlink creation may need Windows developer mode.
#else
    require(!error, "Create synthetic local symlink");
#endif
    rejects([&] { ovmesh::write_waveform_png(utf8(directory / "alias" / "capture.png"), 1, 1, pixels); }, "No parent-symlink capture");
    fs::create_symlink(real / "absent.png", directory / "link.png");
    rejects([&] { ovmesh::write_waveform_png(utf8(directory / "link.png"), 1, 1, pixels); }, "No dangling-symlink capture");
    require(!fs::exists(real / "absent.png") && !fs::exists(real / "capture.png"), "No writes through aliases");
#ifndef _WIN32
    require(::mkfifo((directory / "fifo.png").c_str(), 0600) == 0, "Create synthetic FIFO");
    rejects([&] { ovmesh::write_waveform_png(utf8(directory / "fifo.png"), 1, 1, pixels); }, "No opening or replacing a special file");
#endif
}

void fonts() {
    const auto candidates = ovmesh::system_font_candidates();
    for (const auto* group : {&candidates.ui_sans, &candidates.monospace}) {
        require(group->size() <= 5, "Bounded font candidate inventory");
        std::set<std::string> seen;
        for (const auto& candidate : *group) {
            ovmesh::validate_local_file_path(candidate);
            const fs::path path(std::u8string(candidate.begin(), candidate.end()));
            require(seen.insert(candidate).second, "No duplicate font candidates");
            require(fs::is_regular_file(fs::symlink_status(path)), "Only existing regular local fonts");
            require(fs::file_size(path) <= 20 * 1024 * 1024, "Bounded font input size");
#if defined(__APPLE__)
            require(candidate.starts_with("/System/Library/Fonts/"), "Only OS fonts on macOS");
#elif defined(__linux__)
            require(candidate.starts_with("/usr/share/fonts/"), "Only OS fonts on Linux");
#endif
        }
    }
}
} // namespace

int main() {
    try {
        Fixture fixture;
        roundtrips(fixture.directory);
        invalid_inputs(fixture.directory);
        aliases(fixture.directory);
        fonts();
        std::cout << "Desktop assets: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Desktop assets failed: " << error.what() << '\n';
        return 1;
    }
}
