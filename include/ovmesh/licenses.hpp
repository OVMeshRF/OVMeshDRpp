// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <span>
#include <string_view>

namespace ovmesh {
struct LicenseNotice {
    std::string_view name;
    std::string_view source_path;
    std::string_view text;
};
// Immutable text embedded at build time; no files, URLs or devices are opened.
std::span<const LicenseNotice> license_notices();
}
