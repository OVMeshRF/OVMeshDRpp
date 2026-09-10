// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic measurement fixtures only; no USB, RF samples, keys or operational routes.
#include "storage.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace ovmesh;
namespace fs = std::filesystem;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
bool near(double a, double b, double tolerance = 1e-8) { return std::abs(a - b) <= tolerance; }
std::string contents(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "Read owned synthetic fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}
struct Fixture {
    fs::path directory = fs::current_path() / ("analysis-coverage-fixture-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { require(fs::create_directory(directory), "Create owned fixture directory"); }
    ~Fixture() { std::error_code error; fs::remove_all(directory, error); }
    fs::path file(const char* name) const { return directory / name; }
};
ReceiverConfig config() {
    ReceiverConfig result;
    result.synthetic = true;
    result.center_hz = 1000000000;
    result.sample_rate = 1000000;
    result.survey_span_hz = 1000000;
    result.tuning_offset_hz = 900;
    return result;
}
PositionFix position(double elapsed, double latitude = 0) {
    PositionFix result;
    result.valid = true; result.latitude = latitude; result.longitude = 0;
    result.utc_seconds = 1700000000 + elapsed; result.monotonic_seconds = 100 + elapsed;
    result.source = "synthetic receiver position"; result.hdop = .9; result.satellites = 9;
    return result;
}
SpectrumTile tile(uint64_t id, double elapsed, const std::vector<uint16_t>& masks, double latitude = 0) {
    const auto c = config();
    SpectrumTile result;
    result.id = id;
    // Preserve sample-clock progress across sparse measurement intervals.
    result.first_sample = static_cast<uint64_t>(std::llround(elapsed * c.sample_rate));
    result.frame_count = static_cast<uint32_t>(masks.size());
    result.end_sample = result.first_sample + uint64_t(result.frame_count) * 4096;
    result.elapsed_start_seconds = elapsed;
    result.elapsed_end_seconds = elapsed + double(result.frame_count) * 4096 / c.sample_rate;
    result.utc_start_seconds = 1700000000 + elapsed;
    result.utc_end_seconds = 1700000000 + result.elapsed_end_seconds;
    result.bin_width_hz = double(c.sample_rate) / 4096;
    result.first_center_hz = double(c.center_hz) - 4 * result.bin_width_hz;
    result.background_dbfs = -100;
    result.mean_dbfs.assign(9, -60); result.peak_dbfs.assign(9, -40);
    for (uint16_t mask : masks) {
        result.activity.push_back(static_cast<uint8_t>(mask & 255));
        result.activity.push_back(static_cast<uint8_t>(mask >> 8));
    }
    result.receiver_start = position(elapsed, latitude);
    result.receiver_end = position(result.elapsed_end_seconds, latitude);
    return result;
}
CoverageGap gap(uint64_t id, double from, double to) {
    CoverageGap result;
    result.id = id; result.elapsed_start_seconds = from; result.elapsed_end_seconds = to;
    result.missing_samples = static_cast<uint64_t>(std::llround((to - from) * config().sample_rate));
    result.utc_start_seconds = 1700000000 + from; result.utc_end_seconds = 1700000000 + to;
    result.reason = "synthetic input gap";
    return result;
}
void finish(SessionStore& store, double elapsed, double measured) {
    Snapshot snapshot; snapshot.config = config(); snapshot.elapsed_seconds = elapsed;
    snapshot.measurement_seconds = measured; snapshot.input_seconds = measured;
    store.update(snapshot, true);
}
void conserved(const SurveyAnalysis& analysis) {
    double observed = 0, raw = 0, center = 0, outside = 0;
    double previous_end = -1;
    for (const auto& observation : analysis.observations) {
        require(observation.elapsed_start >= previous_end - 1e-8 && observation.elapsed_end > observation.elapsed_start,
            "Observation buckets stay ordered and nonoverlapping");
        require(observation.observed_seconds <= observation.elapsed_end - observation.elapsed_start + 1e-8,
            "Missing time within a bucket must not become measured time");
        require(observation.busy_seconds <= observation.observed_seconds + 1e-8 &&
            observation.center_busy_seconds <= observation.busy_seconds + 1e-8 &&
            observation.outside_center_busy_seconds <= observation.busy_seconds + 1e-8,
            "Raw and guarded union activity stay within measured time");
        previous_end = observation.elapsed_end;
        observed += observation.observed_seconds; raw += observation.busy_seconds;
        center += observation.center_busy_seconds; outside += observation.outside_center_busy_seconds;
    }
    require(near(observed, analysis.observed_seconds) && near(raw, analysis.busy_seconds) &&
        near(center, analysis.center_busy_seconds) && near(outside, analysis.outside_center_busy_seconds),
        "Display buckets conserve every aggregate duration");
}

void full_history(const Fixture& fixture) {
    constexpr unsigned tile_count = 2005;
    const double duration = 128 * 4096.0 / config().sample_rate;
    const double end = tile_count - 1 + duration;
    const auto path = fixture.file("long-sparse.sqlite");
    {
        SessionStore writer; writer.create(path.string(), config(), "synthetic-complete-history");
        for (unsigned i = 0; i < tile_count; ++i)
            writer.append(tile(i + 1, i, std::vector<uint16_t>(128, i + 1 == tile_count ? 1 : 0), i + 1 == tile_count ? 1 : 0));
        writer.append(gap(1, duration, 1));
        finish(writer, end, tile_count * duration);
    }
    SessionStore reader; reader.open_readonly(path.string());
    const auto analysis = reader.analyze({});
    require(analysis.tile_count == tile_count && analysis.observations.size() <= 2000 && !analysis.observations.empty(),
        "Long history retains every measurement with a bounded display");
    require(analysis.observations_coarsened && !analysis.observations_truncated && analysis.effective_time_bucket_seconds > 1,
        "More than 2000 occupied requested buckets are coarsened rather than truncated");
    require(near(analysis.resolved_elapsed_start, 0) && near(analysis.resolved_elapsed_end, end),
        "Resolved analysis extent reaches the final recorded tile");
    require(near(analysis.observed_seconds, tile_count * duration) && near(analysis.busy_seconds, duration) &&
        near(analysis.outside_center_busy_seconds, duration), "Activity solely after bucket 2000 survives analysis");
    const auto& last = analysis.observations.back();
    require(near(last.elapsed_end, end) && last.busy_seconds > 0 && last.receiver_position && last.receiver_position->latitude == 1,
        "Late activity and its final receiver position survive display coarsening");
    require(analysis.gaps.size() == 1 && analysis.observed_seconds < end, "Sparse and explicit gap time remains unobserved");
    conserved(analysis);
    SurveyQuery coarse; coarse.time_bucket_seconds = 10;
    const auto reference = reader.analyze(coarse);
    require(!reference.observations_coarsened && !reference.observations_truncated &&
        near(reference.observed_seconds, analysis.observed_seconds) && near(reference.busy_seconds, analysis.busy_seconds) &&
        near(reference.outside_center_busy_seconds, analysis.outside_center_busy_seconds),
        "Automatic coarsening preserves totals from an uncapped wide-bucket reference");
    conserved(reference);
    SurveyQuery disabled; disabled.max_observations = 0;
    const auto no_buckets = reader.analyze(disabled);
    require(no_buckets.observations.empty() && no_buckets.observations_truncated &&
        near(no_buckets.observed_seconds, analysis.observed_seconds) && near(no_buckets.busy_seconds, analysis.busy_seconds),
        "A zero display cap suppresses only buckets, not aggregate measurements");
    SurveyQuery region; region.geographic_filter = true; region.south = .9; region.north = 1.1; region.west = -.1; region.east = .1;
    const auto geographic = reader.analyze(region);
    require(geographic.tile_count == 1 && near(geographic.observed_seconds, duration) && near(geographic.busy_seconds, duration) &&
        near(geographic.resolved_elapsed_start, 0) && near(geographic.resolved_elapsed_end, end),
        "Geographic selection retains late position without shrinking the selected time axis");
    require(geographic.gaps.empty(), "Unlocated gaps cannot be assigned to a geographic rectangle");
    conserved(geographic);
    const double frame = 4096.0 / config().sample_rate;
    region.elapsed_start = tile_count - 1 + frame / 4;
    region.elapsed_end = tile_count - 1 + frame * 1.75;
    region.time_bucket_seconds = .001;
    const auto partial = reader.analyze(region);
    require(near(partial.observed_seconds, frame * 1.5) && near(partial.busy_seconds, frame * 1.5) &&
        near(partial.outside_center_busy_seconds, frame * 1.5), "Combined time/geographic filter preserves fractional FFT contributions");
    require(near(partial.resolved_elapsed_start, region.elapsed_start) && near(partial.resolved_elapsed_end, region.elapsed_end),
        "Explicit partial time bounds remain exact");
    conserved(partial);
}

void guard_union_and_readonly(const Fixture& fixture) {
    const auto c = config();
    const double center = double(c.center_hz), width = double(c.sample_rate) / 4096;
    const double frame = 4096.0 / c.sample_rate, duration = frame * 4;
    const auto path = fixture.file("center-and-outside.sqlite");
    {
        SessionStore writer; writer.create(path.string(), c, "synthetic-center-guard");
        // Center in all four frames; two simultaneous outside bins in frames
        // zero and two. Summing per-bin counts would wrongly report 100% outside.
        writer.append(tile(1, 4.3, {uint16_t(16 + 1 + 256), 16, uint16_t(16 + 2 + 128), 16}));
        finish(writer, 4.3 + duration, duration);
    }
    const auto database_before = contents(path);
    {
        SessionStore reader; reader.open_readonly(path.string());
        ExportOptions options; options.include_receiver_positions = true;
        const auto before_export = fixture.file("guard-before.csv"), after_export = fixture.file("guard-after.csv");
        reader.export_csv(before_export.string(), options);
        const auto whole = reader.analyze({});
        require(whole.bins.size() == 9 && whole.center_guard_bin_count == 5 && whole.outside_center_bin_count == 4,
            "Every recorded bin is accounted for inside or outside the five-bin center guard");
        require(near(whole.center_guard_lower_hz, center - 2.5 * width) && near(whole.center_guard_upper_hz, center + 2.5 * width),
            "Guard edges follow nominal FFT bins, not the tuner correction or a guessed RF signal width");
        require(near(whole.busy_seconds, duration) && near(whole.center_busy_seconds, duration) &&
            near(whole.outside_center_busy_seconds, duration / 2), "Raw 100% center activity and outside 50% activity remain distinct union measurements");
        require(near(whole.bins[4].active_seconds, duration) && near(whole.bins[0].active_seconds, frame),
            "Marginal frequency-bin activity remains unmodified");
        conserved(whole);
        SurveyQuery guard; guard.lower_hz = center - 2.5 * width; guard.upper_hz = center + 2.5 * width;
        const auto only_guard = reader.analyze(guard);
        require(only_guard.bins.size() == 5 && only_guard.center_guard_bin_count == 5 && only_guard.outside_center_bin_count == 0 &&
            only_guard.outside_center_busy_seconds == 0 && near(only_guard.busy_seconds, duration),
            "Center-only selection exposes no outside bins so the UI can display N/A");
        conserved(only_guard);
        SurveyQuery outside; outside.lower_hz = center - 4.5 * width; outside.upper_hz = center - 2.5 * width;
        const auto only_outside = reader.analyze(outside);
        require(only_outside.bins.size() == 2 && only_outside.center_guard_bin_count == 0 && only_outside.outside_center_bin_count == 2 &&
            only_outside.center_guard_lower_hz == 0 && only_outside.center_guard_upper_hz == 0 && only_outside.center_busy_seconds == 0 &&
            near(only_outside.busy_seconds, only_outside.outside_center_busy_seconds) && near(only_outside.busy_seconds, duration / 2),
            "Entirely off-center selection retains raw occupancy and has no invented guard span");
        for (int bin_offset : {-3, -2, 2, 3}) {
            SurveyQuery edge; edge.lower_hz = center + bin_offset * width - .1; edge.upper_hz = edge.lower_hz + .2;
            const auto selected = reader.analyze(edge);
            const size_t guarded = std::abs(bin_offset) <= 2 ? 1 : 0;
            require(selected.bins.size() == 1 && selected.center_guard_bin_count == guarded && selected.outside_center_bin_count == 1 - guarded,
                "Guard includes both ±2-bin boundaries and excludes their immediate neighbors");
            conserved(selected);
        }
        SurveyQuery crossing; crossing.lower_hz = center - 2.5 * width - .1; crossing.upper_hz = center - 2.5 * width + .1;
        const auto two_bins = reader.analyze(crossing);
        require(two_bins.bins.size() == 2 && two_bins.center_guard_bin_count == 1 && two_bins.outside_center_bin_count == 1,
            "Selection straddling an FFT edge accounts for both intersecting bins");
        SurveyQuery fine; fine.elapsed_start = 4.3 + frame / 2; fine.elapsed_end = 4.3 + frame * 1.5; fine.time_bucket_seconds = .001;
        const auto fraction = reader.analyze(fine);
        require(near(fraction.observed_seconds, frame) && near(fraction.busy_seconds, frame) &&
            near(fraction.outside_center_busy_seconds, frame / 2), "Boundary-split FFT time preserves fractional outside activity");
        conserved(fraction);
        reader.export_csv(after_export.string(), options);
        require(contents(before_export) == contents(after_export), "Analysis must not change raw exported measurements or schema fields");
    }
    require(contents(path) == database_before, "Retrospective analysis and export leave SQLite bytes unchanged");
}

void gaps_and_small_bounds(const Fixture& fixture) {
    const double frame = 4096.0 / config().sample_rate;
    const auto path = fixture.file("gap-extent.sqlite");
    {
        SessionStore writer; writer.create(path.string(), config(), "synthetic-gap-extent");
        writer.append(tile(1, 10, {0})); writer.append(tile(2, 100, {1}));
        writer.append(gap(1, 0, 10)); writer.append(gap(2, 10 + frame, 100)); writer.append(gap(3, 100 + frame, 200));
        finish(writer, 200, frame * 2);
    }
    SessionStore reader; reader.open_readonly(path.string());
    SurveyQuery one; one.max_observations = 1;
    const auto result = reader.analyze(one);
    require(result.observations.size() == 1 && result.observations_coarsened && !result.observations_truncated &&
        near(result.resolved_elapsed_start, 0) && near(result.resolved_elapsed_end, 200), "Recorded leading/trailing gaps participate in resolved full-session extent");
    require(near(result.observed_seconds, frame * 2) && near(result.busy_seconds, frame) && result.gaps.size() == 3,
        "Coarse display does not count missing time as measured quiet time");
    conserved(result);
    SurveyQuery tiny; tiny.elapsed_start = 100 + frame / 2; tiny.elapsed_end = tiny.elapsed_start + 1e-7; tiny.time_bucket_seconds = .001;
    const auto fraction = reader.analyze(tiny);
    require(fraction.observations.size() == 1 && near(fraction.observed_seconds, tiny.elapsed_end - tiny.elapsed_start, 1e-12) &&
        near(fraction.busy_seconds, fraction.observed_seconds, 1e-12), "Very short positive intervals retain their measured fraction");
    conserved(fraction);
    SurveyQuery no_measurements; no_measurements.elapsed_start = 150; no_measurements.elapsed_end = 180;
    const auto missing = reader.analyze(no_measurements);
    require(missing.observations.empty() && missing.observed_seconds == 0 && missing.busy_seconds == 0 &&
        !missing.observations_truncated && near(missing.resolved_elapsed_start, 150) && near(missing.resolved_elapsed_end, 180),
        "A gap-only time selection has explicit extent and no fabricated zero-occupancy observations");
    const auto empty_path = fixture.file("empty.sqlite");
    { SessionStore writer; writer.create(empty_path.string(), config(), "synthetic-empty-history"); }
    SessionStore empty; empty.open_readonly(empty_path.string());
    const auto no_data = empty.analyze({});
    require(no_data.observations.empty() && no_data.observed_seconds == 0 && !no_data.observations_truncated,
        "Empty recorded history produces no observations");
}

void position_coverage(const Fixture& fixture) {
    const auto path = fixture.file("position-coverage.sqlite");
    const double duration = 4 * 4096.0 / config().sample_rate;
    {
        SessionStore writer; writer.create(path.string(), config(), "synthetic-position-coverage");
        for (unsigned i = 0; i < 4; ++i) {
            auto sample = tile(i + 1, i * duration, {1, 0, 1, 0});
            if (i == 1 || i == 3) sample.receiver_start.reset();
            if (i == 2 || i == 3) sample.receiver_end.reset();
            if (i) sample.quality |= SurveyPositionMissing;
            writer.append(sample);
        }
        finish(writer, 4 * duration, 4 * duration);
    }
    const auto before = contents(path);
    SessionStore reader; reader.open_readonly(path.string());
    const auto all = reader.analyze({});
    require(near(all.missing_start_position_seconds, 2 * duration) &&
        near(all.missing_end_position_seconds, 2 * duration) && near(all.observed_seconds, 4 * duration),
        "Independent start/end missing times preserve RF observed time and overlapping absence");
    conserved(all);
    SurveyQuery partial; partial.elapsed_start = 1.5 * duration; partial.elapsed_end = 2.5 * duration;
    const auto selected = reader.analyze(partial);
    require(near(selected.missing_start_position_seconds, .5 * duration) &&
        near(selected.missing_end_position_seconds, .5 * duration), "Partial time queries clip missing-position durations");
    partial.geographic_filter = true;
    const auto located = reader.analyze(partial);
    require(near(located.observed_seconds, .5 * duration) && located.missing_end_position_seconds == 0 &&
        near(located.missing_start_position_seconds, .5 * duration), "Geographic filters require end fixes without inventing starts");
    require(contents(path) == before, "Position coverage analysis does not repair or rewrite historical fixes");
}
}

int main() {
    try {
        Fixture fixture;
        full_history(fixture);
        guard_union_and_readonly(fixture);
        gaps_and_small_bounds(fixture);
        position_coverage(fixture);
        std::cout << "Complete-history and center-guard analysis checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Analysis coverage checks failed: " << error.what() << '\n';
        return 1;
    }
}
