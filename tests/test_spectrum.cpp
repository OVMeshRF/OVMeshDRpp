// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/spectrum.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iostream>
#include <limits>
#include <numbers>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr size_t n = 4096;
constexpr uint32_t rate = 409600;
constexpr uint64_t center = 915000000;
using Complex = std::complex<float>;
using Processor = ovmesh::SpectrumProcessor;
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance, const char* label) {
    require(std::abs(actual - expected) <= tolerance, std::string(label) +
        ": actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
}
double linear(float value) { return std::pow(10., static_cast<double>(value) / 10.); }

struct Collected {
    std::vector<ovmesh::SpectrumTile> tiles;
    std::vector<ovmesh::SpectrumEvent> events;
    Processor::TileCallback tile_callback() {
        return [this](ovmesh::SpectrumTile tile) { tiles.push_back(std::move(tile)); };
    }
    Processor::EventCallback event_callback() {
        return [this](ovmesh::SpectrumEvent event) { events.push_back(std::move(event)); };
    }
};

std::vector<Complex> frame(std::initializer_list<std::pair<int, float>> tones) {
    std::vector<Complex> result(n);
    // Closed-form complex sinusoids; this source does not call application FFTs,
    // receiver encoders, modulation, windowing or spectrum helper code.
    for (size_t i = 0; i < n; ++i) {
        std::complex<double> sum{};
        for (const auto& [bin, amplitude] : tones)
            sum += std::polar(static_cast<double>(amplitude),
                2 * std::numbers::pi * bin * static_cast<double>(i) / n);
        result[i] = Complex(sum);
    }
    return result;
}
void append(std::vector<Complex>& target, const std::vector<Complex>& values) {
    target.insert(target.end(), values.begin(), values.end());
}
Collected collect(std::span<const Complex> input, uint64_t origin = 0, bool partitioned = false,
                  float threshold = -35, uint32_t span = rate) {
    Processor processor(center, rate, span, threshold);
    Collected result;
    size_t position = 0;
    while (position < input.size()) {
        const size_t amount = partitioned ? std::min(input.size() - position,
            size_t{1} + (position * 127 + 13) % 5317) : input.size();
        processor.feed(input.subspan(position, amount), origin + position,
            result.tile_callback(), result.event_callback());
        position += amount;
    }
    processor.finish(result.tile_callback(), result.event_callback());
    return result;
}
bool active(const ovmesh::SpectrumTile& tile, size_t f, size_t bin) {
    const size_t stride = (tile.mean_dbfs.size() + 7) / 8;
    return (tile.activity[f * stride + bin / 8] & (1u << (bin % 8))) != 0;
}
size_t bin_at(const ovmesh::SpectrumTile& tile, int signed_bin) {
    const auto index = static_cast<size_t>(std::llround(
        (static_cast<double>(center) + signed_bin * 100. - tile.first_center_hz) /
        tile.bin_width_hz));
    require(index < tile.mean_dbfs.size(), "Requested fixture bin is present");
    return index;
}

void independent_tone_power() {
    const auto samples = frame({{123, .5f}});
    const auto result = collect(samples);
    require(result.tiles.size() == 1 && result.events.size() == 1, "One tone tile/event");
    const auto& tile = result.tiles.front();
    require(tile.frame_count == 1 && tile.first_sample == 0 && tile.end_sample == n,
        "Complete FFT observation interval");
    require(tile.mean_dbfs.size() == 4095, "Full span excludes half-outside Nyquist bin");
    near(tile.first_center_hz, center - 204700., 0, "First complete bin center");
    near(tile.bin_width_hz, 100, 0, "Bin width");
    const size_t index = bin_at(tile, 123);
    // Periodic Hann's Fourier series has coefficients 1/2,-1/4,-1/4.
    // Thus a bin-centered complex tone puts 2/3 of its power in the center,
    // 1/6 in each neighbor, and integrates to its squared amplitude.
    near(linear(tile.mean_dbfs[index]), .25 * 2 / 3, 2e-7, "Hann center-bin power");
    near(linear(tile.mean_dbfs[index - 1]), .25 / 6, 1e-7, "Hann lower-bin power");
    near(linear(tile.mean_dbfs[index + 1]), .25 / 6, 1e-7, "Hann upper-bin power");
    double total = 0;
    for (float power : tile.mean_dbfs) total += linear(power);
    near(total, .25, 3e-7, "Integrated tone power");
    require(active(tile, 0, index - 1) && active(tile, 0, index) && active(tile, 0, index + 1),
        "Tone three-bin activity");
    require(!active(tile, 0, index - 2) && !active(tile, 0, index + 2), "No false remote tone activity");
    const auto& event = result.events.front();
    near(event.lower_hz, center + 12150., 0, "Observed lower bin edge");
    near(event.upper_hz, center + 12450., 0, "Observed upper bin edge");
    near(linear(event.mean_dbfs), .25 / 3, 2e-7, "Event active-bin mean power");
    near(linear(event.peak_dbfs), .25 * 2 / 3, 2e-7, "Event strongest bin power");
    near(event.active_seconds, .01, 1e-12, "FFT observation duration");
    require((event.quality & ovmesh::SurveyTruncated) != 0, "End-of-stream tone is truncated");
    Processor processor(center, rate, rate, -35);
    near(processor.enbw_hz(), 150, 1e-5, "Periodic Hann ENBW");
}

void joint_activity_and_duty() {
    const auto a = frame({{-300, .2f}}), b = frame({{300, .2f}});
    const auto both = frame({{-300, .2f}, {300, .2f}}), silent = frame({});
    std::vector<Complex> alternating, simultaneous;
    append(alternating, a); append(alternating, b);
    append(simultaneous, both); append(simultaneous, silent);
    const auto alt = collect(alternating), sim = collect(simultaneous);
    require(alt.tiles.size() == 1 && sim.tiles.size() == 1, "Two-frame 20 ms tiles");
    const auto& x = alt.tiles[0];
    const auto& y = sim.tiles[0];
    const size_t ia = bin_at(x, -300), ib = bin_at(x, 300);
    size_t alt_union = 0, sim_union = 0, alt_a = 0, sim_a = 0;
    for (size_t f = 0; f < 2; ++f) {
        alt_union += active(x, f, ia) || active(x, f, ib);
        sim_union += active(y, f, ia) || active(y, f, ib);
        alt_a += active(x, f, ia);
        sim_a += active(y, f, ia);
    }
    require(alt_union == 2 && sim_union == 1 && alt_a == 1 && sim_a == 1,
        "Joint bitmap distinguishes 100% union occupancy from 50% at equal per-bin duty");
    near(x.mean_dbfs[ia], y.mean_dbfs[ia], 1e-5, "Same duty gives equal mean bin power");
    near(linear(x.mean_dbfs[ia]), .04 / 3, 1e-7, "50% duty mean averaged in linear power");
    near(linear(x.peak_dbfs[ia]), .04 * 2 / 3, 1e-7, "50% duty retains full on-frame peak");
    require(sim.events.size() == 2, "Separated simultaneous frequencies remain separate events");
    for (const auto& event : sim.events)
        require(!(event.quality & ovmesh::SurveyTruncated), "Silence closes completed energy event");

    std::vector<Complex> pulses;
    append(pulses, a); append(pulses, silent); append(pulses, a); append(pulses, silent);
    const auto pulsed = collect(pulses, 1000);
    require(pulsed.events.size() == 2, "Same-frequency pulses separated by silence remain separate");
    require(pulsed.events[0].first_sample == 1000 && pulsed.events[0].end_sample == 1000 + n &&
        pulsed.events[1].first_sample == 1000 + 2 * n, "Pulse timing uses absolute samples");
}

void independent_noise_and_dft() {
    std::mt19937 random(4271);
    std::normal_distribution<float> normal(0, .025f);
    std::vector<Complex> samples(64 * n);
    for (auto& sample : samples) sample = {normal(random), normal(random)};
    const auto result = collect(samples, 0, true, -30);
    double sum = 0, measured_background = 0;
    for (const auto& tile : result.tiles) {
        for (float bin : tile.mean_dbfs) sum += linear(bin) * tile.frame_count;
        measured_background += linear(tile.background_dbfs);
        require((tile.quality & ovmesh::SurveyBackgroundUncertain) != 0,
            "Background estimate carries uncertainty");
    }
    near(sum / 64, 2 * .025 * .025, .00002, "White complex noise integrates to component variances");
    require(result.events.empty(), "Noise below fixed threshold does not create events");
    measured_background /= static_cast<double>(result.tiles.size());
    // Two averaged independent exponential periodograms have a gamma(2) law;
    // its 20th percentile is about 0.412 times the mean white-noise bin power.
    near(measured_background / (2 * .025 * .025 / n), .412, .035,
        "Spectral background follows averaged-noise percentile, not total noise");

    const auto first = collect(std::span(samples).first(n));
    for (int bin : {-1677, -91, 0, 734}) {
        std::complex<double> dft{};
        double window_energy = 0;
        // Independent direct O(N) DFT at selected frequencies validates indexing,
        // FFT sign and normalization against noise with unrelated Fourier content.
        for (size_t i = 0; i < n; ++i) {
            const double w = .5 - .5 * std::cos(2 * std::numbers::pi * static_cast<double>(i) / n);
            dft += std::complex<double>(samples[i]) * w * std::polar(1.,
                -2 * std::numbers::pi * bin * static_cast<double>(i) / n);
            window_energy += w * w;
        }
        const double expected = std::norm(dft) / (n * window_energy);
        near(linear(first.tiles[0].mean_dbfs[bin_at(first.tiles[0], bin)]), expected,
            expected * 5e-6 + 1e-12, "Independent selected-bin noise DFT");
    }
}

void partition_gap_tail_clipping() {
    std::vector<Complex> input;
    const auto a = frame({{19, .3f}}), silent = frame({});
    for (unsigned f = 0; f < 7; ++f) append(input, f == 3 ? silent : a);
    input.resize(input.size() + 71, {1, 0});
    const auto whole = collect(input, 500), parts = collect(input, 500, true);
    require(whole.tiles.size() == parts.tiles.size() && whole.events.size() == parts.events.size(),
        "Partition invariant tile/event counts");
    for (size_t i = 0; i < whole.tiles.size(); ++i) {
        const auto& a_tile = whole.tiles[i];
        const auto& b_tile = parts.tiles[i];
        require(a_tile.first_sample == b_tile.first_sample && a_tile.end_sample == b_tile.end_sample &&
            a_tile.mean_dbfs == b_tile.mean_dbfs && a_tile.peak_dbfs == b_tile.peak_dbfs &&
            a_tile.activity == b_tile.activity && a_tile.clipped_samples == 0,
            "Partition invariant complete-FFT values; clipped tail excluded");
    }
    for (size_t i = 0; i < whole.events.size(); ++i)
        require(whole.events[i].first_sample == parts.events[i].first_sample &&
            whole.events[i].end_sample == parts.events[i].end_sample &&
            whole.events[i].mean_dbfs == parts.events[i].mean_dbfs,
            "Partition invariant event measurements");

    Processor processor(center, rate, rate, -35);
    Collected result;
    processor.feed(std::span(input).first(n + 71), 12345, result.tile_callback(), result.event_callback());
    require(processor.pending_samples() == 71, "Partial FFT exposed before gap");
    bool rejected = false;
    try { processor.feed(a, 99999, result.tile_callback(), result.event_callback()); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && processor.pending_samples() == 71 && result.tiles.empty(),
        "Discontinuity rejected without changing pending data");
    processor.gap(result.tile_callback(), result.event_callback());
    require(processor.dropped_partial_samples() == 71 && processor.pending_samples() == 0,
        "Gap discloses dropped tail");
    processor.feed(a, 99999, result.tile_callback(), result.event_callback());
    processor.finish(result.tile_callback(), result.event_callback());
    require(result.tiles.size() == 2 && result.events.size() == 2 &&
        result.tiles[0].end_sample == 12345 + n && result.tiles[1].first_sample == 99999,
        "Gap never joins FFTs or events across missing samples");
    require(result.tiles[1].id > result.tiles[0].id && result.events[1].id > result.events[0].id,
        "IDs monotonic across gaps");
    require((result.events[0].quality & ovmesh::SurveyTruncated) != 0, "Gap marks event truncation");
    near(result.tiles[1].elapsed_start_seconds, 99999. / rate, 1e-12, "Absolute sample offset to elapsed time");
    processor.finish(result.tile_callback(), result.event_callback());
    require(result.tiles.size() == 2, "Finish is idempotent");
    rejected = false;
    try { processor.feed(a, 99999 + n, result.tile_callback(), result.event_callback()); }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected, "Finished stream cannot silently reopen");

    auto clipped = silent;
    clipped[1010] = {127.f / 128, 0};
    clipped[1520] = {-1, 0};
    clipped[2030] = {0, -127.f / 128};
    clipped[3040] = {1, -1}; // One complex sample, despite both components clipping.
    clipped[2550] = {126.f / 128, 126.f / 128};
    const auto counts = collect(clipped, 0, false, -110);
    require(counts.tiles[0].clipped_samples == 4 &&
        (counts.tiles[0].quality & ovmesh::SurveyClipped) != 0, "Both signed clipping rails counted once per complex sample");
    require(!counts.events.empty() && (counts.events[0].quality & ovmesh::SurveyClipped) != 0,
        "Clipped frames still yield flagged measurements");
}

void width_edges_and_bounds() {
    const auto narrow = collect(frame({{0, .2f}}));
    const auto wide = collect(frame({{-4, .1f}, {-2, .1f}, {0, .1f}, {2, .1f}, {4, .1f}}));
    require(narrow.events.size() == 1 && wide.events.size() == 1, "Contiguous multitone envelope is one energy event");
    near(narrow.events[0].upper_hz - narrow.events[0].lower_hz, 300, 0, "Narrow observed envelope");
    near(wide.events[0].upper_hz - wide.events[0].lower_hz, 1100, 0, "Wider observed envelope");
    const auto edge = collect(frame({{9, .2f}, {13, .2f}}), 0, false, -35, 2000);
    require(edge.tiles[0].mean_dbfs.size() == 19, "Only whole bins inside requested span");
    near(edge.tiles[0].first_center_hz, center - 900., 0, "Narrow-span bin center");
    require(edge.events.size() == 1 && (edge.events[0].quality & ovmesh::SurveyBoundary) != 0,
        "Clipped frequency envelope marks usable-span boundary and excludes outside tone");
    near(edge.events[0].upper_hz, center + 950., 0, "Observed upper envelope stays inside usable span");

    Processor processor(center, rate, rate, -35);
    Collected result;
    const auto steady = frame({{27, .2f}});
    for (size_t i = 0; i < 405; ++i)
        processor.feed(steady, i * n, result.tile_callback(), result.event_callback());
    processor.finish(result.tile_callback(), result.event_callback());
    require(result.events.size() == 3, "Persistent signal is segmented at bounded duration");
    double observed = 0;
    for (const auto& event : result.events) {
        require((event.quality & ovmesh::SurveyTruncated) != 0 && event.active_seconds <= 2.000001,
            "Every bounded segment discloses truncation");
        observed += event.active_seconds;
    }
    near(observed, 4.05, 1e-10, "Segmentation preserves steady-signal observation duration");
    for (const auto& tile : result.tiles)
        for (size_t f = 0; f < tile.frame_count; ++f)
            require(active(tile, f, bin_at(tile, 27)), "Persistent signal is never learned into inactive background");

    std::vector<Complex> many(n);
    for (size_t i = 0; i < n; ++i) {
        std::complex<double> value{};
        // 100 separated tones exercise the bounded run policy, with unrelated
        // phases to avoid forcing all tones to peak at the same input sample.
        for (int tone = 0; tone < 100; ++tone)
            value += std::polar(.002, 2 * std::numbers::pi * (-1500 + tone * 30) *
                static_cast<double>(i) / n + tone * tone * .371);
        many[i] = Complex(value);
    }
    const auto dense = collect(many, 0, false, -65);
    require(!dense.events.empty() && dense.events.size() <= Processor::max_tracks,
        "Concurrent track state remains bounded");
    for (const auto& event : dense.events)
        require((event.quality & ovmesh::SurveyMerged) && (event.quality & ovmesh::SurveyTruncated),
            "Track-limit grouping discloses merged/truncated frequency separation");
    size_t active_bins = 0;
    for (size_t i = 0; i < dense.tiles[0].mean_dbfs.size(); ++i)
        active_bins += active(dense.tiles[0], 0, i);
    require(active_bins == 300, "Track limit preserves all independent bitmap activity");

    std::vector<Complex> merging;
    append(merging, frame({{-2, .2f}, {2, .2f}}));
    append(merging, frame({{0, .2f}}));
    append(merging, frame({}));
    const auto merged = collect(merging);
    require(merged.events.size() == 1 && (merged.events[0].quality & ovmesh::SurveyMerged),
        "Converging frequency components form a visibly merged energy event");
    near(merged.events[0].active_seconds, .02, 1e-12,
        "Merged simultaneous histories count their time union once");
    near(merged.events[0].upper_hz - merged.events[0].lower_hz, 700, 0,
        "Merged event retains complete observed envelope");
    near(linear(merged.events[0].mean_dbfs), .12 / 9, 1e-7,
        "Merged power mean retains all active bin measurements exactly once");

    Processor fast(center, 100000000, 16000000, -35);
    Collected bounded;
    const auto silent = frame({});
    for (size_t i = 0; i < 129; ++i)
        fast.feed(silent, i * n, bounded.tile_callback(), bounded.event_callback());
    fast.finish(bounded.tile_callback(), bounded.event_callback());
    require(bounded.tiles.size() == 2 && bounded.tiles[0].frame_count == 128 &&
        bounded.tiles[1].frame_count == 1, "High-rate tile memory is capped at 128 FFTs");
}

void silence_and_invalid_input() {
    std::vector<Complex> silence(3 * n + 9);
    const auto result = collect(silence, 100);
    require(result.tiles.size() == 2 && result.events.empty(), "Silence still provides complete coverage tiles");
    for (const auto& tile : result.tiles) {
        require(tile.background_dbfs == -180 && tile.clipped_samples == 0, "Silent noise estimate is finite floor");
        require(std::all_of(tile.activity.begin(), tile.activity.end(), [](uint8_t value) { return value == 0; }),
            "Silence has zero joint activity");
        require(std::all_of(tile.mean_dbfs.begin(), tile.mean_dbfs.end(), [](float value) { return value == -180; }),
            "Silence has finite power floor");
    }
    for (const auto& configuration : std::array<std::pair<uint32_t, uint32_t>, 4>{
            {{0, 1000}, {rate, 0}, {rate, rate + 1}, {rate, 99}}}) {
        bool rejected = false;
        try { Processor invalid(center, configuration.first, configuration.second, -35); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Invalid or less-than-one-bin span rejected");
    }
    Processor processor(center, rate, rate, -35);
    Collected output;
    auto invalid = frame({});
    invalid.back() = {std::numeric_limits<float>::quiet_NaN(), 0};
    bool rejected = false;
    try { processor.feed(invalid, 0, output.tile_callback(), output.event_callback()); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && processor.pending_samples() == 0, "Nonfinite input rejected before mutating stream");
    processor.feed(std::span(silence).first(9), 0, output.tile_callback(), output.event_callback());
    processor.finish(output.tile_callback(), output.event_callback());
    require(output.tiles.empty() && output.events.empty() && processor.dropped_partial_samples() == 9,
        "Entire sub-FFT stream is disclosed as unmeasured tail");
}
} // namespace

int main() {
    try {
        independent_tone_power();
        joint_activity_and_duty();
        independent_noise_and_dft();
        partition_gap_tail_clipping();
        width_edges_and_bounds();
        silence_and_invalid_input();
        std::cout << "Spectrum tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Spectrum tests failed: " << error.what() << '\n';
        return 1;
    }
}
