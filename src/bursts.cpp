// SPDX-License-Identifier: GPL-3.0-or-later
#include "bursts.hpp"
#include <algorithm>
#include <bitset>
#include <cmath>
#include <stdexcept>

namespace ovmesh {
void BurstGrouper::finish(const Callback& emit, bool limited) {
    for (auto& track : tracks_) { track.burst.limited |= limited; emit(track.burst); }
    tracks_.clear(); previous_end_ = -1;
}

void BurstGrouper::consume(const SpectrumTile& tile, const Callback& emit,
                          size_t first, size_t last, double from, double to) {
    const size_t bins = tile.mean_dbfs.size(), stride = (bins + 7) / 8;
    if (!bins || bins > 4096 || !tile.frame_count || tile.frame_count > 128 ||
        tile.peak_dbfs.size() != bins || tile.activity.size() != stride * tile.frame_count ||
        !std::isfinite(tile.bin_width_hz) || tile.bin_width_hz <= 0 ||
        !std::isfinite(tile.first_center_hz) ||
        !std::isfinite(tile.elapsed_start_seconds) || !std::isfinite(tile.elapsed_end_seconds) ||
        tile.elapsed_end_seconds <= tile.elapsed_start_seconds)
        throw std::runtime_error("Invalid burst measurement tile");
    last = std::min(last, bins);
    from = std::max(from, tile.elapsed_start_seconds);
    to = to < 0 ? tile.elapsed_end_seconds : std::min(to, tile.elapsed_end_seconds);
    if (first >= last || to <= from) { finish(emit); return; }
    if (previous_end_ >= 0 && (std::abs(from - previous_end_) > 1e-8 ||
        grid_bins_ != bins || grid_first_ != tile.first_center_hz || grid_width_ != tile.bin_width_hz))
        finish(emit); // Never group across unobserved time or a grid change.
    previous_end_ = to; grid_bins_ = bins; grid_first_ = tile.first_center_hz; grid_width_ = tile.bin_width_hz;
    for (size_t i = 0; i < tracks_.size();) {
        if (from - tracks_[i].burst.elapsed_end_seconds > .040 + 1e-8) {
            emit(tracks_[i].burst); tracks_.erase(tracks_.begin() + static_cast<std::ptrdiff_t>(i));
        } else ++i;
    }
    const double step = (tile.elapsed_end_seconds - tile.elapsed_start_seconds) / tile.frame_count;
    std::vector<std::bitset<128>> activity(bins);
    for (size_t f = 0; f < tile.frame_count; ++f) {
        const double start = tile.elapsed_start_seconds + double(f) * step;
        if (start >= to || start + step <= from) continue;
        for (size_t b = first; b < last; ++b)
            if (tile.activity[f * stride + b / 8] & (1u << (b % 8))) activity[b].set(f);
    }
    const auto guard = [&](size_t b) {
        return std::abs(tile.first_center_hz + double(b) * tile.bin_width_hz - center_) <= 2 * tile.bin_width_hz + 1e-5;
    };
    // Make tile components before matching, so separate simultaneous runs
    // cannot silently grow the same existing track in one tile.
    struct Component { SpectrumBurst burst; std::bitset<128> frames; };
    std::vector<Component> components;
    for (size_t b = first; b < last;) {
        // A single FFT crossing is insufficient to estimate a signal's band.
        // Keep it in the unchanged fragment/mask evidence, not as a separate
        // candidate "channel". Repeated visits support a sweeping signal.
        if (activity[b].count() < 2) { ++b; continue; }
        const size_t low = b; size_t high = b; const bool center = guard(b);
        std::bitset<128> frames = activity[b]; bool bridged = false;
        for (size_t next = b + 1; next < last && guard(next) == center; ++next) {
            if (next > high + 3) break;
            if (activity[next].count() >= 2) {
                bridged |= next > high + 1; high = next; frames |= activity[next];
            }
        }
        SpectrumBurst burst; burst.components = 1; burst.center_region = center;
        burst.ambiguous = bridged;
        burst.lower_hz = tile.first_center_hz + (double(low) - .5) * tile.bin_width_hz;
        burst.upper_hz = tile.first_center_hz + (double(high) + .5) * tile.bin_width_hz;
        burst.quality = tile.quality;
        if (center) burst.quality |= SurveyDcSuspect;
        if (from > tile.elapsed_start_seconds || to < tile.elapsed_end_seconds || low == first || high + 1 == last)
            burst.quality |= SurveyBoundary;
        bool started = false;
        for (size_t f = 0; f < tile.frame_count; ++f) if (frames[f]) {
            const double start = std::max(from, tile.elapsed_start_seconds + double(f) * step);
            const double end = std::min(to, tile.elapsed_start_seconds + double(f + 1) * step);
            if (!started) { burst.elapsed_start_seconds = start; started = true; }
            burst.elapsed_end_seconds = end; burst.active_seconds += end - start;
        }
        for (size_t i = low; i <= high; ++i) burst.peak_dbfs = std::max(burst.peak_dbfs, tile.peak_dbfs[i]);
        components.push_back({burst, frames}); b = high + 1;
    }
    const size_t old_count = tracks_.size();
    std::vector<bool> retire(old_count);
    std::vector<std::vector<size_t>> matches(components.size());
    std::vector<size_t> uses(old_count);
    const double padding = 2 * tile.bin_width_hz + 1e-5;
    for (size_t i = 0; i < components.size(); ++i)
        for (size_t j = 0; j < old_count; ++j) {
            const auto& c = components[i].burst; const auto& t = tracks_[j];
            if (c.elapsed_start_seconds - t.burst.elapsed_end_seconds <= .040 + 1e-8 &&
                c.center_region == t.burst.center_region && c.lower_hz <= t.last_high + padding &&
                c.upper_hz >= t.last_low - padding) { matches[i].push_back(j); ++uses[j]; }
        }
    std::vector<bool> absorbed(components.size());
    for (size_t j = 0; j < old_count; ++j) if (uses[j] > 1) {
        std::vector<size_t> pieces; std::bitset<128> frames, overlap;
        bool safe = true;
        for (size_t i = 0; i < components.size(); ++i)
            if (std::find(matches[i].begin(),matches[i].end(),j) != matches[i].end()) {
                const auto& c=components[i];
                safe &= matches[i].size()==1 && c.burst.lower_hz >= tracks_[j].burst.lower_hz-padding &&
                    c.burst.upper_hz <= tracks_[j].burst.upper_hz+padding;
                overlap |= frames & c.frames; frames |= c.frames; pieces.push_back(i);
            }
        // Preserve a previously established band through a frequency sweep's
        // disjoint pieces. Simultaneous separated components are not evidence
        // of the same sweep, nor are components claimed by another track.
        if (!safe || overlap.count()*4 > frames.count()) continue;
        auto& combined=components[pieces.front()]; combined.frames=frames;
        for (size_t k=1;k<pieces.size();++k) {
            const size_t i=pieces[k]; const auto& b=components[i].burst; absorbed[i]=true;
            combined.burst.lower_hz=std::min(combined.burst.lower_hz,b.lower_hz);
            combined.burst.upper_hz=std::max(combined.burst.upper_hz,b.upper_hz);
            combined.burst.peak_dbfs=std::max(combined.burst.peak_dbfs,b.peak_dbfs);
            combined.burst.quality|=b.quality;
        }
        combined.burst.ambiguous=true; // Temporal association remains a hypothesis.
        combined.burst.active_seconds=0; bool started=false;
        for(size_t f=0;f<tile.frame_count;++f)if(frames[f]) {
            const double start=std::max(from,tile.elapsed_start_seconds+double(f)*step);
            const double end=std::min(to,tile.elapsed_start_seconds+double(f+1)*step);
            if(!started){combined.burst.elapsed_start_seconds=start;started=true;}
            combined.burst.elapsed_end_seconds=end;combined.burst.active_seconds+=end-start;
        }
        uses[j]=1;
    }
    for (size_t i = 0; i < components.size(); ++i) {
        if(absorbed[i])continue;
        auto c = components[i].burst;
        if (matches[i].size() == 1 && uses[matches[i][0]] == 1) {
            auto& t = tracks_[matches[i][0]];
            t.burst.elapsed_end_seconds = c.elapsed_end_seconds;
            t.burst.lower_hz = std::min(t.burst.lower_hz, c.lower_hz);
            t.burst.upper_hz = std::max(t.burst.upper_hz, c.upper_hz);
            t.burst.active_seconds += c.active_seconds; ++t.burst.components;
            t.burst.peak_dbfs = std::max(t.burst.peak_dbfs, c.peak_dbfs);
            t.burst.quality |= c.quality; t.burst.ambiguous |= c.ambiguous;
            t.last_low = c.lower_hz; t.last_high = c.upper_hz; t.seen = to;
        } else {
            c.id = next_id_++; c.ambiguous |= !matches[i].empty();
            for (auto j : matches[i]) { tracks_[j].burst.ambiguous = true; retire[j] = true; }
            if (tracks_.size() < 128) tracks_.push_back({c, c.lower_hz, c.upper_hz, to});
            else { c.limited = true; emit(c); }
        }
    }
    // Close ambiguous predecessors once. Keeping them eligible for the next
    // tiles creates repeated split/merge cascades from a single transition.
    for (size_t j = old_count; j > 0; --j) if (retire[j-1]) {
        emit(tracks_[j-1].burst); tracks_.erase(tracks_.begin()+static_cast<std::ptrdiff_t>(j-1));
    }
    for (size_t i = 0; i < tracks_.size();) {
        if (tracks_[i].burst.elapsed_end_seconds - tracks_[i].burst.elapsed_start_seconds >= 10) {
            tracks_[i].burst.limited = true; emit(tracks_[i].burst);
            tracks_.erase(tracks_.begin() + static_cast<std::ptrdiff_t>(i));
        } else ++i;
    }
}
}
