// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ovmesh::discovery_detail {

// Storage starts zeroed. Between wipes, writes follow a contiguous logical
// sample range; wraparound replaces older entries. Clear every slot touched by
// that range while leaving never-written slots alone. Call before resetting the
// range metadata. The returned count describes actual bounded clearing work.
inline std::size_t wipe_ring(std::span<std::complex<float>> ring,
                             std::uint64_t first_written,
                             std::uint64_t written_count) noexcept {
    if (ring.empty() || written_count == 0) return 0;
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(written_count, ring.size()));
    const auto first = static_cast<std::size_t>(first_written % ring.size());
    const auto initial = std::min(count, ring.size() - first);
    std::fill_n(ring.subspan(first).begin(), initial, std::complex<float>{});
    std::fill_n(ring.begin(), count - initial, std::complex<float>{});
    return count;
}

} // namespace ovmesh::discovery_detail
