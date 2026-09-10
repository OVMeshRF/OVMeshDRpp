// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ovmesh/survey.hpp"
#include <complex>
#include <functional>
#include <memory>
#include <span>

namespace ovmesh {
// Bounded, phase-free spectral measurements from every complete contiguous
// 4096-sample FFT. Periodic Hann, no overlap or sample skipping. Bin power is
// |FFT|^2 / (N * sum(window^2)); a unit complex tone has total power 0 dBFS.
// Hann ENBW is 1.5 * bin_width_hz(). Bins whose edges leave the usable span
// are excluded. No antenna/gain calibration is implied by dBFS measurements.
//
// Tiles contain ceil(20 ms / FFT duration) frames (at most 128), and can end
// early on finish/gap. Mean is averaged in linear power; peak is per bin.
// background_dbfs is the 20th percentile across the tile's mean bin powers,
// an uncertain spectral background estimate, not a measured noise floor for
// each bin. Activity uses a fixed absolute threshold, including steady signals.
// The frame-major activity bitmap preserves simultaneous versus alternating
// activity for exact temporal union occupancy at FFT resolution.
//
// Events connect adjacent active frequency bins and intersecting/touching
// envelopes in successive FFTs. Their mean is the linear mean over active
// bin/frame measurements; peak is the strongest individual bin measurement.
// Duration/active_seconds is the connected event's observed frame interval.
// Merging histories is marked; frequency envelopes may contain inactive bins.
// Events are generic energy observations, never packets or source identities.
// At most 64 tracks survive a frame; excess separated runs are grouped with
// Merged|Truncated flags. Persistent events split after about two seconds,
// with Truncated on both sides. No event or raw-IQ history grows unbounded.
//
// first_sample is an absolute coordinate. Discontinuities throw unless gap()
// is called first. finish/gap emit complete FFTs, mark unfinished events as
// Truncated, discard the partial FFT, and increase dropped_partial_samples().
// pending_samples() allows the caller to record that precise coverage gap.
// finish closes the stream; gap permits a new absolute origin and keeps IDs.
// Times here are sample/rate offsets. The engine owns UTC, epochs and GPS.
class SpectrumProcessor {
public:
    static constexpr uint32_t fft_size = 4096;
    static constexpr size_t max_tracks = 64;
    static constexpr uint32_t max_tile_frames = 128;
    using TileCallback = std::function<void(SpectrumTile)>;
    using EventCallback = std::function<void(SpectrumEvent)>;

    SpectrumProcessor(uint64_t center_hz, uint32_t sample_rate,
                      uint32_t span_hz, float threshold_dbfs);
    ~SpectrumProcessor();
    SpectrumProcessor(SpectrumProcessor&&) noexcept;
    SpectrumProcessor& operator=(SpectrumProcessor&&) noexcept;
    SpectrumProcessor(const SpectrumProcessor&) = delete;
    SpectrumProcessor& operator=(const SpectrumProcessor&) = delete;

    void feed(std::span<const std::complex<float>> input, uint64_t first_sample,
              const TileCallback& tile, const EventCallback& event);
    void finish(const TileCallback& tile, const EventCallback& event);
    void gap(const TileCallback& tile, const EventCallback& event);
    size_t pending_samples() const;
    uint64_t dropped_partial_samples() const;
    size_t bin_count() const;
    double first_center_hz() const;
    double bin_width_hz() const;
    double enbw_hz() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ovmesh
