// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ovmesh/survey.hpp"
#include <functional>

namespace ovmesh {
// Version 1 grouping: >=2 active FFTs per seed bin in a recorded ~20 ms tile, <=2 empty bins
// between components, <=40 ms between tiles, <=10 s per output segment.
// The receiver-center guard is always a separate class. Measurements unchanged.
class BurstGrouper {
public:
    using Callback = std::function<void(SpectrumBurst)>;
    explicit BurstGrouper(double center_hz) : center_(center_hz) {}
    void consume(const SpectrumTile&, const Callback&, size_t first = 0,
                 size_t last = SIZE_MAX, double from = -1, double to = -1);
    void finish(const Callback&, bool limited = true);
private:
    struct Track { SpectrumBurst burst; double last_low = 0, last_high = 0, seen = 0; };
    double center_, previous_end_ = -1, grid_first_ = 0, grid_width_ = 0;
    size_t grid_bins_ = 0;
    uint64_t next_id_ = 1;
    std::vector<Track> tracks_;
};
}
