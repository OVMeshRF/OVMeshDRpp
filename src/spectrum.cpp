// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectrum.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace ovmesh {
namespace {
constexpr double minimum_power = 1e-18;
float dbfs(double power) {
    return static_cast<float>(10.0 * std::log10(std::max(power, minimum_power)));
}
bool touches(size_t a, size_t b, size_t c, size_t d) {
    return a <= d + 1 && c <= b + 1;
}
} // namespace

class SpectrumProcessor::Impl {
public:
    struct Run {
        size_t low = 0, high = 0;
        double sum = 0, peak = 0;
        uint64_t count = 0;
        uint32_t quality = 0;
    };
    struct Track {
        uint64_t first = 0, end = 0;
        size_t low = 0, high = 0, last_low = 0, last_high = 0;
        double sum = 0, peak = 0;
        uint64_t count = 0;
        uint32_t quality = SurveyUncalibrated | SurveyBackgroundUncertain;
    };

    uint32_t rate, frames_per_tile;
    double width, first_center, enbw, normalization, threshold;
    int first_signed_bin;
    size_t bins, bytes_per_frame;
    uint64_t segment_samples, next_sample = 0, frame_first = 0;
    uint64_t discarded = 0, next_tile_id = 1, next_event_id = 1, frame_clipped = 0;
    size_t pending = 0;
    bool have_origin = false, finished = false;
    std::array<float, fft_size> window{};
    std::array<size_t, fft_size> reversal{};
    std::array<std::complex<float>, fft_size / 2> twiddles{};
    std::array<std::complex<float>, fft_size> fft{};
    std::vector<double> powers, tile_sum, tile_peak, background_scratch;
    std::vector<Run> runs;
    std::vector<Track> tracks, next_tracks;
    std::vector<std::pair<size_t, size_t>> split_intervals;
    SpectrumTile tile;

    Impl(uint64_t center, uint32_t sample_rate, uint32_t span, float cutoff)
        : rate(sample_rate) {
        if (!rate || !span || span > rate || !std::isfinite(cutoff) ||
            cutoff < -180 || cutoff > 20)
            throw std::invalid_argument("Invalid spectrum rate, span or absolute dBFS threshold");
        width = static_cast<double>(rate) / fft_size;
        first_signed_bin = std::max(-static_cast<int>(fft_size / 2),
            static_cast<int>(std::ceil(-static_cast<double>(span) / (2 * width) + .5)));
        const int last_bin = std::min(static_cast<int>(fft_size / 2) - 1,
            static_cast<int>(std::floor(static_cast<double>(span) / (2 * width) - .5)));
        if (last_bin < first_signed_bin)
            throw std::invalid_argument("Spectrum span contains no complete FFT bin");
        bins = static_cast<size_t>(last_bin - first_signed_bin + 1);
        first_center = static_cast<double>(center) + first_signed_bin * width;
        bytes_per_frame = (bins + 7) / 8;
        frames_per_tile = static_cast<uint32_t>(std::clamp(
            std::ceil(.020 * rate / fft_size), 1.0, static_cast<double>(max_tile_frames)));
        segment_samples = static_cast<uint64_t>(std::ceil(2.0 * rate / fft_size)) * fft_size;
        threshold = std::pow(10.0, static_cast<double>(cutoff) / 10);
        double sum_w = 0, sum_w2 = 0;
        for (size_t i = 0; i < fft_size; ++i) {
            window[i] = static_cast<float>(.5 - .5 * std::cos(
                2 * std::numbers::pi * static_cast<double>(i) / fft_size));
            sum_w += window[i];
            sum_w2 += static_cast<double>(window[i]) * window[i];
            size_t value = i, reversed = 0;
            for (unsigned bit = 0; bit < 12; ++bit) {
                reversed = (reversed << 1) | (value & 1);
                value >>= 1;
            }
            reversal[i] = reversed;
        }
        normalization = 1.0 / (fft_size * sum_w2);
        enbw = rate * sum_w2 / (sum_w * sum_w);
        for (size_t i = 0; i < twiddles.size(); ++i)
            twiddles[i] = std::polar(1.0f, static_cast<float>(
                -2 * std::numbers::pi * static_cast<double>(i) / fft_size));
        powers.resize(bins);
        tile_sum.resize(bins);
        tile_peak.resize(bins);
        background_scratch.resize(bins);
        runs.reserve(fft_size / 2);
        tracks.reserve(max_tracks);
        next_tracks.reserve(max_tracks);
        split_intervals.reserve(max_tracks);
        prepare_tile();
    }

    void prepare_tile() {
        tile = {};
        tile.fft_size = fft_size;
        tile.first_center_hz = first_center;
        tile.bin_width_hz = width;
        tile.quality = SurveyUncalibrated | SurveyBackgroundUncertain;
        tile.activity.reserve(bytes_per_frame * frames_per_tile);
        std::fill(tile_sum.begin(), tile_sum.end(), 0);
        std::fill(tile_peak.begin(), tile_peak.end(), 0);
    }

    void emit_tile(const TileCallback& callback) {
        if (!tile.frame_count) return;
        tile.id = next_tile_id++;
        tile.elapsed_start_seconds = static_cast<double>(tile.first_sample) / rate;
        tile.elapsed_end_seconds = static_cast<double>(tile.end_sample) / rate;
        tile.mean_dbfs.resize(bins);
        tile.peak_dbfs.resize(bins);
        for (size_t i = 0; i < bins; ++i) {
            background_scratch[i] = tile_sum[i] / tile.frame_count;
            tile.mean_dbfs[i] = dbfs(background_scratch[i]);
            tile.peak_dbfs[i] = dbfs(tile_peak[i]);
        }
        const size_t percentile = (bins - 1) / 5;
        std::nth_element(background_scratch.begin(), background_scratch.begin() +
            static_cast<std::ptrdiff_t>(percentile), background_scratch.end());
        tile.background_dbfs = dbfs(background_scratch[percentile]);
        if (callback) callback(std::move(tile));
        prepare_tile();
    }

    void emit_event(const Track& track, const EventCallback& callback, uint32_t quality = 0) {
        SpectrumEvent event;
        event.id = next_event_id++;
        event.first_sample = track.first;
        event.end_sample = track.end;
        event.elapsed_start_seconds = static_cast<double>(track.first) / rate;
        event.elapsed_end_seconds = static_cast<double>(track.end) / rate;
        event.active_seconds = static_cast<double>(track.end - track.first) / rate;
        event.lower_hz = first_center + (static_cast<double>(track.low) - .5) * width;
        event.upper_hz = first_center + (static_cast<double>(track.high) + .5) * width;
        event.mean_dbfs = dbfs(track.sum / static_cast<double>(track.count));
        event.peak_dbfs = dbfs(track.peak);
        event.quality = track.quality | quality;
        if (callback) callback(std::move(event));
    }

    void events(uint64_t first, const EventCallback& callback) {
        runs.clear();
        for (size_t i = 0; i < bins;) {
            if (powers[i] < threshold) { ++i; continue; }
            Run run;
            run.low = i;
            do {
                run.sum += powers[i];
                run.peak = std::max(run.peak, powers[i]);
                ++run.count;
                ++i;
            } while (i < bins && powers[i] >= threshold);
            run.high = i - 1;
            if (!run.low || run.high + 1 == bins) run.quality |= SurveyBoundary;
            if (frame_clipped) run.quality |= SurveyClipped;
            runs.push_back(run);
        }
        if (runs.size() > max_tracks) {
            // Preserve all active power and the observed envelope while bounding
            // association cost. The bitmap still preserves exact separated bins.
            const size_t group_size = (runs.size() + max_tracks - 1) / max_tracks;
            size_t out = 0;
            for (size_t begin = 0; begin < runs.size(); begin += group_size) {
                Run merged = runs[begin];
                for (size_t j = begin + 1; j < std::min(begin + group_size, runs.size()); ++j) {
                    merged.high = runs[j].high;
                    merged.sum += runs[j].sum;
                    merged.peak = std::max(merged.peak, runs[j].peak);
                    merged.count += runs[j].count;
                    merged.quality |= runs[j].quality;
                }
                merged.quality |= SurveyMerged | SurveyTruncated;
                runs[out++] = merged;
            }
            runs.resize(out);
        }
        // Connected components in the bipartite graph of last-frame tracks and
        // this frame's runs. A split/merge is represented by one marked envelope.
        std::array<size_t, max_tracks * 2> parent{};
        std::iota(parent.begin(), parent.end(), size_t{0});
        auto root = [&](size_t node) {
            while (parent[node] != node) {
                parent[node] = parent[parent[node]];
                node = parent[node];
            }
            return node;
        };
        for (size_t i = 0; i < tracks.size(); ++i)
            for (size_t j = 0; j < runs.size(); ++j)
                if (touches(tracks[i].last_low, tracks[i].last_high, runs[j].low, runs[j].high))
                    parent[root(i)] = root(max_tracks + j);

        std::array<bool, max_tracks> old_used{};
        next_tracks.clear();
        for (size_t j = 0; j < runs.size(); ++j) {
            const size_t group = root(max_tracks + j);
            bool seen = false;
            for (size_t k = 0; k < j; ++k)
                if (root(max_tracks + k) == group) { seen = true; break; }
            if (seen) continue;
            Track track;
            track.first = first;
            track.end = first + fft_size;
            track.low = track.last_low = bins;
            size_t old_count = 0, new_count = 0;
            for (size_t i = 0; i < tracks.size(); ++i) {
                if (root(i) != group) continue;
                old_used[i] = true;
                ++old_count;
                const auto& old = tracks[i];
                track.first = std::min(track.first, old.first);
                track.low = std::min(track.low, old.low);
                track.high = std::max(track.high, old.high);
                track.sum += old.sum;
                track.peak = std::max(track.peak, old.peak);
                track.count += old.count;
                track.quality |= old.quality;
            }
            for (size_t k = j; k < runs.size(); ++k) {
                if (root(max_tracks + k) != group) continue;
                ++new_count;
                const auto& run = runs[k];
                track.low = std::min(track.low, run.low);
                track.high = std::max(track.high, run.high);
                track.last_low = std::min(track.last_low, run.low);
                track.last_high = std::max(track.last_high, run.high);
                track.sum += run.sum;
                track.peak = std::max(track.peak, run.peak);
                track.count += run.count;
                track.quality |= run.quality;
                for (const auto& interval : split_intervals)
                    if (touches(interval.first, interval.second, run.low, run.high))
                        track.quality |= SurveyTruncated;
            }
            if (old_count > 1 || new_count > 1) track.quality |= SurveyMerged;
            next_tracks.push_back(track);
        }
        for (size_t i = 0; i < tracks.size(); ++i)
            if (!old_used[i]) emit_event(tracks[i], callback);
        tracks.clear();
        split_intervals.clear();
        for (const auto& track : next_tracks) {
            if (track.end - track.first >= segment_samples) {
                emit_event(track, callback, SurveyTruncated);
                split_intervals.emplace_back(track.last_low, track.last_high);
            } else tracks.push_back(track);
        }
    }

    void process_frame(const TileCallback& tile_callback, const EventCallback& event_callback) {
        for (size_t length = 2; length <= fft_size; length *= 2) {
            const size_t half = length / 2, stride = fft_size / length;
            for (size_t start = 0; start < fft_size; start += length) {
                for (size_t j = 0; j < half; ++j) {
                    const auto even = fft[start + j];
                    const auto odd = fft[start + j + half] * twiddles[j * stride];
                    fft[start + j] = even + odd;
                    fft[start + j + half] = even - odd;
                }
            }
        }
        if (!tile.frame_count) tile.first_sample = frame_first;
        tile.end_sample = frame_first + fft_size;
        const size_t activity_start = tile.activity.size();
        tile.activity.resize(activity_start + bytes_per_frame, 0);
        for (size_t i = 0; i < bins; ++i) {
            const int signed_bin = first_signed_bin + static_cast<int>(i);
            const size_t fft_bin = static_cast<size_t>(signed_bin < 0 ?
                signed_bin + static_cast<int>(fft_size) : signed_bin);
            powers[i] = static_cast<double>(std::norm(fft[fft_bin])) * normalization;
            tile_sum[i] += powers[i];
            tile_peak[i] = std::max(tile_peak[i], powers[i]);
            if (powers[i] >= threshold)
                tile.activity[activity_start + i / 8] |= static_cast<uint8_t>(1u << (i % 8));
        }
        ++tile.frame_count;
        tile.clipped_samples += frame_clipped;
        if (frame_clipped) tile.quality |= SurveyClipped;
        events(frame_first, event_callback);
        if (tile.frame_count == frames_per_tile) emit_tile(tile_callback);
        frame_clipped = 0;
    }

    void flush(const TileCallback& tile_callback, const EventCallback& event_callback) {
        emit_tile(tile_callback);
        for (const auto& track : tracks) emit_event(track, event_callback, SurveyTruncated);
        tracks.clear();
        split_intervals.clear();
        discarded += pending;
        pending = 0;
        frame_clipped = 0;
        std::fill(fft.begin(), fft.end(), std::complex<float>{});
        have_origin = false;
    }
};

SpectrumProcessor::SpectrumProcessor(uint64_t center, uint32_t rate, uint32_t span, float cutoff)
    : impl_(std::make_unique<Impl>(center, rate, span, cutoff)) {}
SpectrumProcessor::~SpectrumProcessor() = default;
SpectrumProcessor::SpectrumProcessor(SpectrumProcessor&&) noexcept = default;
SpectrumProcessor& SpectrumProcessor::operator=(SpectrumProcessor&&) noexcept = default;

void SpectrumProcessor::feed(std::span<const std::complex<float>> input, uint64_t first_sample,
                             const TileCallback& tile, const EventCallback& event) {
    auto& state = *impl_;
    if (state.finished) throw std::logic_error("Spectrum stream already finished");
    if (input.empty()) return;
    if (input.size() > std::numeric_limits<uint64_t>::max() - first_sample)
        throw std::invalid_argument("Spectrum sample coordinate overflow");
    if (state.have_origin && first_sample != state.next_sample)
        throw std::invalid_argument("Spectrum sample discontinuity requires gap()");
    for (const auto value : input)
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
            throw std::invalid_argument("Spectrum input must be finite");
    state.have_origin = true;
    for (size_t i = 0; i < input.size(); ++i) {
        if (!state.pending) state.frame_first = first_sample + i;
        const auto value = input[i];
        if (std::abs(value.real()) >= 127.0f / 128 || std::abs(value.imag()) >= 127.0f / 128)
            ++state.frame_clipped;
        state.fft[state.reversal[state.pending]] = value * state.window[state.pending];
        if (++state.pending == fft_size) {
            state.pending = 0;
            state.process_frame(tile, event);
        }
    }
    state.next_sample = first_sample + input.size();
}

void SpectrumProcessor::finish(const TileCallback& tile, const EventCallback& event) {
    if (impl_->finished) return;
    impl_->flush(tile, event);
    impl_->finished = true;
}
void SpectrumProcessor::gap(const TileCallback& tile, const EventCallback& event) {
    if (impl_->finished) throw std::logic_error("Spectrum stream already finished");
    impl_->flush(tile, event);
}
size_t SpectrumProcessor::pending_samples() const { return impl_->pending; }
uint64_t SpectrumProcessor::dropped_partial_samples() const { return impl_->discarded; }
size_t SpectrumProcessor::bin_count() const { return impl_->bins; }
double SpectrumProcessor::first_center_hz() const { return impl_->first_center; }
double SpectrumProcessor::bin_width_hz() const { return impl_->width; }
double SpectrumProcessor::enbw_hz() const { return impl_->enbw; }
} // namespace ovmesh
