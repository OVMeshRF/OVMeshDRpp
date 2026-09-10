// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/licenses.hpp"
#include "license_notices.inc"

namespace ovmesh {
std::span<const LicenseNotice> license_notices() { return bundled_notices; }
}
