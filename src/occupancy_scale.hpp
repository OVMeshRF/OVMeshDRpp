// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>

namespace ovmesh {
// Display transform only: retain true zero and the original measured ratios.
// Shared by the desktop and standalone report so brief activity stays visible.
inline double low_activity_height(double ratio) {
    if (!std::isfinite(ratio)) return 0;
    return std::log1p(std::clamp(ratio, 0.0, 1.0) * 1e6) / std::log1p(1e6);
}
}
