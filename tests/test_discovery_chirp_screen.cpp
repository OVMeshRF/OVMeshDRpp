// SPDX-License-Identifier: GPL-3.0-or-later
#include "discovery_chirp_screen.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <iostream>
#include <limits>
#include <numbers>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using C = std::complex<float>;
using D = std::complex<double>;
using Screen = ovmesh::DiscoveryChirpScreen;
constexpr std::array<std::uint32_t, 5> widths{15625, 62500, 125000, 250000, 500000};
constexpr double pi = std::numbers::pi;
void require(bool okay, const char* text) { if (!okay) throw std::runtime_error(text); }

struct Hypothesis {
    std::uint32_t bw;
    unsigned sf;
    std::size_t n;
    std::array<std::vector<C>, 4> references;
};
const std::vector<Hypothesis>& hypotheses() {
    static const auto list = [] {
        std::vector<Hypothesis> result;
        for (auto bw : widths) for (unsigned sf = 7; sf <= (bw == 15625u ? 10u : 12u); ++sf) {
            Hypothesis h{bw, sf, (2000000 / bw) * (std::size_t{1} << sf), {}};
            for (std::size_t lag = 0; lag < h.references.size(); ++lag) {
                auto& row = h.references[lag]; row.resize((8u << lag) * 2000000 / bw);
                for (std::size_t i = 0; i < row.size(); ++i) {
                    const double angle = -2 * pi * static_cast<double>(i) / static_cast<double>(row.size());
                    row[i] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
                }
            }
            result.push_back(std::move(h));
        }
        return result;
    }();
    return list;
}

// Exact prior component-detector loop: double products and accumulators,
// float reference coefficients, local index i=0,4,...,N-4.
Screen::Statistic brute(std::span<const C> input, std::uint64_t origin,
                        std::uint64_t end, const Hypothesis& h, std::size_t lag) {
    Screen::Statistic result;
    const auto delay = h.n / (8u << lag); result.count = h.n / 4;
    if (end < origin || end - origin < h.n + delay || end % (h.n / 4)) return result;
    for (std::size_t i = 0; i < h.n; i += 4) {
        const auto at = static_cast<std::size_t>(end - h.n - origin) + i;
        const auto z = D(input[at]) * std::conj(D(input[at - delay]));
        result.correlation += z * D(h.references[lag][i % h.references[lag].size()]);
        result.dc += z; result.energy += std::norm(z);
    }
    result.residual = std::max(0., result.energy - std::norm(result.dc) / static_cast<double>(result.count));
    result.available = true;
    return result;
}

std::vector<C> fixture(std::size_t size, unsigned kind) {
    std::vector<C> result(size);
    std::mt19937 random(8723401u + kind);
    auto noise = [&] { return static_cast<float>(static_cast<double>(random()) / 4294967296. - .5); };
    for (std::size_t i = 0; i < size; ++i) {
        const double seconds = static_cast<double>(i) / 2000000.;
        const double local = std::fmod(seconds, .008192);
        const double chirp_cycles = -125000 * local + 125000 / .008192 * local * local + 39127 * seconds;
        const double chirp_phase = 2 * pi * std::remainder(chirp_cycles, 1.);
        const double cw_phase = 2 * pi * std::remainder(230271 * seconds, 1.);
        const C chirp{static_cast<float>(std::cos(chirp_phase)), static_cast<float>(std::sin(chirp_phase))};
        const C cw{static_cast<float>(std::cos(cw_phase)), static_cast<float>(std::sin(cw_phase))};
        if (kind == 0) result[i] = C{noise(), noise()} * .2f;
        if (kind == 1) result[i] = cw * .2f;
        if (kind == 3) result[i] = cw * .2f + C{noise(), noise()} * .0000005f;
        if (kind == 2) {
            // Chirp+CW+noise, partial fading and an entirely quiet interval
            // longer than every statistic's complete history.
            const float fade = i % 73000 < 57000 ? .2f : .003f;
            result[i] = chirp * fade + cw * .13f + C{noise(), noise()} * .015f;
            if (i > size / 3 && i < size / 3 + 150000) result[i] = {};
        }
    }
    return result;
}

struct Comparison {
    double correlation_error = 0, dc_error = 0, energy_error = 0, residual_error = 0;
    std::uint64_t statistics = 0, decisions = 0, differing_statistics = 0, differing_decisions = 0;
    std::uint64_t recomputations = 0;
};
void compare(std::span<const C> input, std::uint64_t origin, Comparison& comparison, Screen& screen) {
    for (std::size_t i = 0; i < input.size(); ++i) {
        screen.push(input[i], origin + i);
        const auto end = origin + i + 1;
        if (end % 128) continue;
        for (const auto& h : hypotheses()) {
            if (end - origin < 2 * h.n || end % (h.n / 4)) continue;
            bool expected_pass = false;
            for (std::size_t lag = 0; lag < 4; ++lag) {
                const auto expected = brute(input, origin, end, h, lag);
                const auto got = screen.statistic(h.bw, h.sf, lag);
                require(expected.available && got.available, "Rolling screen omitted eligible observed history");
                const double correlation_scale = std::max(1., std::sqrt(expected.energy * static_cast<double>(expected.count)));
                const double energy_scale = std::max(1., expected.energy);
                comparison.correlation_error = std::max(comparison.correlation_error,
                    std::abs(got.correlation - expected.correlation) / correlation_scale);
                comparison.dc_error = std::max(comparison.dc_error, std::abs(got.dc - expected.dc) / correlation_scale);
                comparison.energy_error = std::max(comparison.energy_error, std::abs(got.energy - expected.energy) / energy_scale);
                comparison.residual_error = std::max(comparison.residual_error, std::abs(got.residual - expected.residual) / energy_scale);
                require(std::isfinite(got.energy) && std::isfinite(got.residual) &&
                        std::isfinite(got.correlation.real()) && std::isfinite(got.correlation.imag()),
                        "Rolling screen returned non-finite statistics");
                ++comparison.statistics;
                if (got.passes() != expected.passes()) ++comparison.differing_statistics;
                expected_pass |= expected.passes();
            }
            ++comparison.decisions;
            if (screen.passes(h.bw, h.sf) != expected_pass) ++comparison.differing_decisions;
        }
    }
    comparison.recomputations += screen.recomputation_count();
}

void compact_history_boundaries(Comparison& comparison) {
    Screen screen;
    const auto input = fixture(16385, 0);
    // A reused physical ring must match a new direct window after every reset,
    // including intervals with zero stored samples and either partial endpoint.
    for (std::uint64_t residue = 0; residue < 4; ++residue) {
        for (std::uint64_t length = 0; length < 8; ++length) {
            screen.reset();
            for (std::uint64_t i = 0; i < length; ++i)
                screen.push({.625f, -.375f}, residue + i);
            screen.reset();
            compare(input, 1024 + residue, comparison, screen);
        }
    }
    screen.reset();
    const auto origin = std::numeric_limits<std::uint64_t>::max() - input.size();
    compare(input, origin, comparison, screen);
    // end_ is UINT64_MAX here; ceil(end_/4) must not wrap while wiping.
    screen.reset();
    compare(input, 3, comparison, screen);

    // A discontinuity and invalid values on discarded sample phases must still
    // clear all previous IQ/statistics; storage compaction never skips validation.
    for (std::uint64_t residue = 1; residue < 4; ++residue) {
        bool rejected = false;
        try { screen.push({}, 32 + residue); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Nonselected-phase gap was accepted");
        compare(input, 2048 + residue, comparison, screen);
        screen.reset();
        screen.push({1, 0}, 0);
        for (std::uint64_t i = 1; i < residue; ++i) screen.push({}, i);
        rejected = false;
        try { screen.push({0, std::numeric_limits<float>::infinity()}, residue); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && !screen.statistic(500000, 7, 0).available,
                "Nonselected non-finite input was accepted or retained state");
        compare(input, 4096 + residue, comparison, screen);
    }
}

void invalid_and_readiness() {
    Screen screen;
    const auto input = fixture(10000, 0);
    for (std::size_t i = 0; i < 575; ++i) screen.push(input[i], i);
    require(!screen.statistic(500000, 7, 0).available, "Incomplete differential history appears observed");
    screen.push(input[575], 575);
    require(!screen.statistic(500000, 7, 0).available, "Off-cadence differential query appears eligible");
    for (std::size_t i = 576; i < 640; ++i) screen.push(input[i], i);
    require(screen.statistic(500000, 7, 0).available, "First complete eligible differential window is unavailable");
    bool rejected = false;
    try { screen.push(input[0], 999); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && !screen.statistic(500000, 7, 0).available, "Gap must reject and clear old history");
    for (std::size_t i = 0; i < 10000; ++i) screen.push(input[i], 711 + i);
    screen.reset();
    require(!screen.passes(250000, 11), "Reset retains a chirp-screen decision");
    for (auto value : {C{std::numeric_limits<float>::quiet_NaN(), 0}, C{0, std::numeric_limits<float>::infinity()}}) {
        rejected = false;
        try { screen.push(value, 0); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && !screen.statistic(500000, 7, 0).available, "Non-finite input must clear differential state");
    }
    rejected = false;
    try { screen.push({}, std::numeric_limits<std::uint64_t>::max()); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Wrapping sample coordinate was accepted");
    for (auto bw : {0u, 123456u}) {
        rejected = false; try { (void)screen.passes(bw, 7); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Invalid bandwidth was accepted");
    }
    rejected = false; try { (void)screen.statistic(250000, 6, 0); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Invalid SF was accepted");
    rejected = false; try { (void)screen.statistic(250000, 11, 4); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Invalid lag was accepted");
}

void benchmark(unsigned kind) {
    const auto input = fixture(1000000, kind);
    Screen screen;
    std::uint64_t rolling = 0, rescanning = 0, queries = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < input.size(); ++i) {
        screen.push(input[i], i); const auto end = i + 1;
        if (end % 128) continue;
        for (const auto& h : hypotheses()) if (end >= 2 * h.n && !(end % (h.n / 4))) {
            rolling += screen.passes(h.bw, h.sf); ++queries;
        }
    }
    const auto middle = std::chrono::steady_clock::now();
    for (std::size_t end = 128; end <= input.size(); end += 128)
        for (const auto& h : hypotheses()) if (end >= 2 * h.n && !(end % (h.n / 4))) {
            for (std::size_t lag = 0; lag < 4; ++lag) if (brute(input, 0, end, h, lag).passes()) {
                ++rescanning; break;
            }
        }
    const auto finish = std::chrono::steady_clock::now();
    const double rolling_seconds = std::chrono::duration<double>(middle - start).count();
    const double rescan_seconds = std::chrono::duration<double>(finish - middle).count();
    std::cout << "kind=" << kind << " input_s=.5 rolling_s=" << rolling_seconds << " rescan_s=" << rescan_seconds
              << " speedup=" << rescan_seconds / rolling_seconds << " eligible_queries=" << queries
              << " rolling_passes=" << rolling << " rescan_passes=" << rescanning << '\n';
    std::cout << "Cancellation recomputations=" << screen.recomputation_count() << '\n';
    require(rolling == rescanning, "Benchmark screen decisions differ");
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
            std::cout << "Rolling includes input handling; rescans use already retained samples. Both check all 28 hypotheses at N/4 cadence and short-circuit passing lags.\n";
            for (unsigned kind = 0; kind < 4; ++kind) benchmark(kind);
            return 0;
        }
        require(argc == 1, "Usage: test_discovery_chirp_screen [--benchmark]");
        Comparison comparison;
        Screen screen;
        for (unsigned kind = 0; kind < 4; ++kind) {
            screen.reset();
            compare(fixture(600001, kind), kind == 0 ? 0 : 916 + kind, comparison, screen);
        }
        compact_history_boundaries(comparison);
        invalid_and_readiness();
        std::cout << "statistics=" << comparison.statistics << " eligible_decisions=" << comparison.decisions
                  << " per_lag_decision_differences=" << comparison.differing_statistics
                  << " hypothesis_decision_differences=" << comparison.differing_decisions
                  << " cancellation_recomputations=" << comparison.recomputations
                  << " normalized_correlation_error=" << comparison.correlation_error
                  << " normalized_dc_error=" << comparison.dc_error
                  << " normalized_energy_error=" << comparison.energy_error
                  << " normalized_residual_error=" << comparison.residual_error << '\n';
        require(comparison.correlation_error < 1e-8 && comparison.dc_error < 1e-8 &&
                comparison.energy_error < 1e-8 && comparison.residual_error < 1e-8,
                "Rolling differential statistics exceed the numerical error budget");
        require(!comparison.differing_statistics && !comparison.differing_decisions,
                "Rolling differential screen changed decisions for the bounded validation fixtures");
        std::cout << "Four-lag rolling differential-screen checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
