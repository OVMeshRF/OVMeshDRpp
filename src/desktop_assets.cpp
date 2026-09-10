// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop_assets.hpp"
#include "ovmesh/file_chooser.hpp"
#include "ovmesh/local_paths.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ovmesh {
namespace {
namespace fs = std::filesystem;

std::string utf8(const fs::path& path) {
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}

void add_font(std::vector<std::string>& candidates, const fs::path& path) {
    try {
        const auto name = utf8(path);
        // The same policy rejects network mounts and aliases before any font
        // is offered to the renderer. Missing fonts simply use the next choice.
        validate_local_file_path(name);
        std::error_code error;
        if (!fs::is_regular_file(fs::symlink_status(path, error)) || error) return;
        const auto size = fs::file_size(path, error);
        if (error || size < 12 || size > 20 * 1024 * 1024) return;
        candidates.push_back(name);
    } catch (const std::exception&) {
        // A machine without a permitted local font still has the built-in font.
    }
}

void check_png_name(const std::string& destination) {
    validate_local_file_path(destination);
    const fs::path path(std::u8string(destination.begin(), destination.end()));
    // Reuse the chooser's portable UTF-8 filename checks, then exclusively
    // create again below so a file appearing after validation is not replaced.
    (void)choose_local_file(utf8(path.parent_path()), utf8(path.filename()), FileChoiceKind::Png, false);
    auto extension = utf8(path.extension());
    for (auto& character : extension)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    if (extension != ".png") throw std::runtime_error("Choose a new .png filename");
}

// Same private/exclusive creation boundary as the session and report writers.
class PngFile {
public:
    explicit PngFile(const std::string& path) : path_(path) {
        check_png_name(path);
#ifdef _WIN32
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)", SDDL_REVISION_1,
                                                                   &descriptor, nullptr))
            throw std::runtime_error("Cannot prepare private PNG permissions");
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        handle_ = CreateFileW(fs::path(std::u8string(path.begin(), path.end())).c_str(),
                              GENERIC_WRITE | DELETE, 0, &security, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        LocalFree(descriptor);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot create PNG; choose a new local filename");
#else
        descriptor_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor_ < 0) throw std::runtime_error("Cannot create PNG; choose a new local filename");
#endif
    }
    ~PngFile() {
        if (!complete_) discard();
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (descriptor_ >= 0) ::close(descriptor_);
#endif
    }
    PngFile(const PngFile&) = delete;
    PngFile& operator=(const PngFile&) = delete;

    void write(std::span<const uint8_t> data) {
        size_t offset = 0;
        while (offset < data.size()) {
#ifdef _WIN32
            DWORD written = 0;
            if (!WriteFile(handle_, data.data() + offset,
                           static_cast<DWORD>(std::min<size_t>(data.size() - offset, 65536)),
                           &written, nullptr) || written == 0)
                throw std::runtime_error("PNG write failed");
#else
            const auto written = ::write(descriptor_, data.data() + offset, data.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::runtime_error("PNG write failed");
#endif
            offset += static_cast<size_t>(written);
        }
    }
    void finish() {
#ifdef _WIN32
        if (!FlushFileBuffers(handle_)) throw std::runtime_error("PNG flush failed");
#else
        if (::fsync(descriptor_) != 0) throw std::runtime_error("PNG flush failed");
#endif
        complete_ = true;
    }

private:
    void discard() noexcept {
#ifdef _WIN32
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (handle_ != INVALID_HANDLE_VALUE)
            SetFileInformationByHandle(handle_, FileDispositionInfo, &disposition, sizeof(disposition));
#else
        struct stat held{}, named{};
        if (descriptor_ >= 0 && ::fstat(descriptor_, &held) == 0 && ::lstat(path_.c_str(), &named) == 0 &&
            held.st_dev == named.st_dev && held.st_ino == named.st_ino) ::unlink(path_.c_str());
#endif
    }
    std::string path_;
    bool complete_ = false;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

constexpr auto crc_table = [] {
    std::array<uint32_t, 256> table{};
    for (uint32_t index = 0; index < table.size(); ++index) {
        uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0U);
        table[index] = value;
    }
    return table;
}();

std::array<uint8_t, 4> big_endian(uint32_t value) {
    return {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
            static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
}

void chunk(PngFile& file, const std::array<uint8_t, 4>& type, std::span<const uint8_t> bytes) {
    file.write(big_endian(static_cast<uint32_t>(bytes.size())));
    file.write(type);
    file.write(bytes);
    uint32_t crc = 0xffffffffU;
    for (const auto byte : type) crc = crc_table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    for (const auto byte : bytes) crc = crc_table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    file.write(big_endian(crc ^ 0xffffffffU));
}

// PNG filter 0, RGBA8, and a zlib stream of stored DEFLATE blocks. This small
// encoder intentionally trades compression for no extra dependency. It holds
// only one 64 KiB block in addition to the caller's crop; it records no metadata.
class PixelStream {
public:
    PixelStream(PngFile& file, uint64_t size) : file_(file), remaining_(size) {
        const std::array<uint8_t, 2> header{0x78, 0x01};
        chunk(file_, {'I', 'D', 'A', 'T'}, header);
    }
    void append(std::span<const uint8_t> bytes) {
        while (!bytes.empty()) {
            const size_t count = std::min(bytes.size(), block_.size() - used_);
            const auto part = bytes.first(count);
            std::copy(part.begin(), part.end(), block_.begin() + static_cast<ptrdiff_t>(used_));
            // Reduce frequently enough that both Adler sums fit in uint32_t.
            for (size_t offset = 0; offset < part.size();) {
                const size_t end = std::min(offset + 5552, part.size());
                for (; offset < end; ++offset) { first_ += part[offset]; second_ += first_; }
                first_ %= 65521;
                second_ %= 65521;
            }
            used_ += count;
            remaining_ -= count;
            bytes = bytes.subspan(count);
            if (used_ == block_.size() || remaining_ == 0) flush();
        }
    }
    void finish() {
        if (remaining_ != 0 || used_ != 5) throw std::runtime_error("PNG pixel stream is incomplete");
        chunk(file_, {'I', 'D', 'A', 'T'}, big_endian((second_ << 16) | first_));
    }
private:
    void flush() {
        const auto count = static_cast<uint16_t>(used_ - 5);
        const auto inverse = static_cast<uint16_t>(~count);
        block_[0] = remaining_ == 0 ? 1 : 0; // BFINAL; BTYPE = stored, byte-aligned.
        block_[1] = static_cast<uint8_t>(count);
        block_[2] = static_cast<uint8_t>(count >> 8);
        block_[3] = static_cast<uint8_t>(inverse);
        block_[4] = static_cast<uint8_t>(inverse >> 8);
        chunk(file_, {'I', 'D', 'A', 'T'}, std::span(block_.data(), used_));
        used_ = 5;
    }
    PngFile& file_;
    std::array<uint8_t, 65540> block_{};
    size_t used_ = 5;
    uint64_t remaining_;
    uint32_t first_ = 1, second_ = 0;
};
} // namespace

DesktopFontCandidates system_font_candidates() {
    DesktopFontCandidates result;
#ifdef _WIN32
    std::array<wchar_t, 32768> directory{};
    const auto length = GetWindowsDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size()) return result;
    const fs::path folder = fs::path(directory.data()) / L"Fonts";
    for (const auto* name : {L"segoeui.ttf", L"arial.ttf", L"tahoma.ttf"}) add_font(result.ui_sans, folder / name);
    for (const auto* name : {L"consola.ttf", L"cour.ttf"}) add_font(result.monospace, folder / name);
#elif defined(__APPLE__)
    for (const auto* path : {"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc",
                             "/System/Library/Fonts/Supplemental/Arial.ttf"}) add_font(result.ui_sans, path);
    for (const auto* path : {"/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/SFNSMono.ttf",
                             "/System/Library/Fonts/Monaco.ttf"}) add_font(result.monospace, path);
#elif defined(__linux__)
    for (const auto* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                             "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                             "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
                             "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
                             "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"}) add_font(result.ui_sans, path);
    for (const auto* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                             "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
                             "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf",
                             "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf"}) add_font(result.monospace, path);
#endif
    return result;
}

void write_waveform_png(const std::string& destination, uint32_t width, uint32_t height,
                        std::span<const uint8_t> rgba, bool bottom_up) {
    const uint64_t pixels = static_cast<uint64_t>(width) * height;
    if (width == 0 || height == 0 || width > waveform_png_max_dimension || height > waveform_png_max_dimension ||
        pixels > waveform_png_max_pixels || rgba.size() != pixels * 4)
        throw std::runtime_error("PNG requires a bounded, tightly packed RGBA plot crop");
    PngFile file(destination);
    const std::array<uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    file.write(signature);
    std::array<uint8_t, 13> header{};
    const auto encoded_width = big_endian(width), encoded_height = big_endian(height);
    std::copy(encoded_width.begin(), encoded_width.end(), header.begin());
    std::copy(encoded_height.begin(), encoded_height.end(), header.begin() + 4);
    header[8] = 8; // Bit depth.
    header[9] = 6; // Truecolor with alpha; default compression/filter/interlace.
    chunk(file, {'I', 'H', 'D', 'R'}, header);
    const size_t row_bytes = static_cast<size_t>(width) * 4;
    PixelStream stream(file, pixels * 4 + height);
    const std::array<uint8_t, 1> filter{0};
    for (uint32_t row = 0; row < height; ++row) {
        stream.append(filter);
        const uint32_t source_row = bottom_up ? height - row - 1 : row;
        stream.append(rgba.subspan(static_cast<size_t>(source_row) * row_bytes, row_bytes));
    }
    stream.finish();
    chunk(file, {'I', 'E', 'N', 'D'}, {});
    file.finish();
}

} // namespace ovmesh
