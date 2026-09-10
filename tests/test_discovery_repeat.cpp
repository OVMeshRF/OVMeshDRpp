// SPDX-License-Identifier: GPL-3.0-or-later
#include "discovery_repeat.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <iostream>
#include <limits>
#include <numbers>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using Complex = std::complex<float>;
using Detector = ovmesh::DiscoveryRepeat;
constexpr double pi = std::numbers::pi;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

double reference(const std::vector<Complex>& input, size_t end, size_t lag) {
    if (end < 2 * lag) return 0;
    std::complex<double> sum{};
    double a = 0, b = 0;
    for (size_t i = 0; i < lag; ++i) {
        const auto x = input[end - lag + i], y = input[end - 2 * lag + i];
        sum += std::complex<double>(x * std::conj(y));
        a += std::norm(x); b += std::norm(y);
    }
    return a * b > 1e-30 ? std::sqrt(std::max(0., std::norm(sum) / (a * b) - 1. / static_cast<double>(lag))) : 0;
}

std::vector<Complex> fixture(size_t count, unsigned kind) {
    std::vector<Complex> input(count);
    std::mt19937 random(73129);
    auto noise = [&] { return static_cast<float>(static_cast<double>(random()) / 4294967296. - .5); };
    for (size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / 2e6;
        const double local = std::fmod(t, .008192);
        const double cycles = kind == 1 ? 187321. * t :
            125000. * local * local / .008192 - 125000. * local + 21923. * t;
        const auto signal = std::polar(.2f, static_cast<float>(2 * pi * std::remainder(cycles, 1.)));
        if (kind == 0) input[i] = {noise(), noise()};
        else if (kind == 1 || kind == 2) input[i] = signal + Complex(noise() * .02f, noise() * .02f);
        // Exercise the vanishing-energy branch and ring replacement in one
        // stream, rather than relying only on a permanently zero fixture.
        if (kind == 3 || (kind == 2 && i > count / 2 && i < count / 2 + 150000)) input[i] = {};
    }
    return input;
}

double check(const std::vector<Complex>& input, size_t chunk) {
    Detector detector;
    double max_error = 0;
    for (size_t begin = 0; begin < input.size(); begin += chunk) {
        const auto end = std::min(begin + chunk, input.size());
        for (size_t i = begin; i < end; ++i) detector.push(input[i]);
        for (size_t lag = Detector::min_lag; lag <= Detector::max_lag; lag *= 2) {
            const auto got = detector.coherence(lag);
            const auto expected = reference(input, end, lag);
            require(std::isfinite(got), "Rolling statistic is non-finite");
            max_error = std::max(max_error, std::abs(got - expected));
            require(std::abs(got - expected) < 2e-8, "Rolling repetition differs from exact-window reference");
        }
    }
    return max_error;
}

void reset_checks() {
    Detector detector;
    const auto input = fixture(140000, 1);
    for (auto value : input) detector.push(value);
    detector.reset();
    require(detector.coherence(65536) == 0, "Reset retains repetition evidence");
    for (size_t i = 0; i < 1023; ++i) detector.push(input[i]);
    require(detector.coherence(512) == 0, "Incomplete pair of windows is treated as observed");
    detector.push(input[1023]);
    require(std::abs(detector.coherence(512) - reference(input, 1024, 512)) < 2e-8,
            "First complete pair of windows differs after reset");
    for (auto invalid : {Complex(std::numeric_limits<float>::quiet_NaN(), 0),
                         Complex(std::numeric_limits<float>::infinity(), 0),
                         Complex(std::numeric_limits<float>::max(), 0)}) {
        bool rejected = false;
        try { detector.push(invalid); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && detector.coherence(512) == 0, "Invalid input was accepted or retained history");
    }
    for (size_t lag : {0u, 511u, 513u, 131072u}) {
        bool rejected = false;
        try { (void)detector.coherence(lag); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Unsupported repetition lag was accepted");
    }
}

void benchmark() {
    const auto input = fixture(2000000, 0);
    double rolling_checksum = 0, reference_checksum = 0;
    Detector detector;
    const auto begin = std::chrono::steady_clock::now();
    for (size_t i = 0; i < input.size(); ++i) {
        detector.push(input[i]);
        for (size_t lag = 512; lag <= 65536; lag *= 2)
            if (i + 1 >= 2 * lag && (i + 1) % (lag / 4) == 0) rolling_checksum += detector.coherence(lag);
    }
    const auto middle = std::chrono::steady_clock::now();
    for (size_t end = 128; end <= input.size(); end += 128)
        for (size_t lag = 512; lag <= 65536; lag *= 2)
            if (end >= 2 * lag && end % (lag / 4) == 0) reference_checksum += reference(input, end, lag);
    const auto finish = std::chrono::steady_clock::now();
    const double rolling_seconds = std::chrono::duration<double>(middle - begin).count();
    const double reference_seconds = std::chrono::duration<double>(finish - middle).count();
    require(std::abs(rolling_checksum - reference_checksum) < 1e-5, "Benchmark statistics differ");
    std::cout << "2 MS/s, one second, eight lags at their N/4 checkpoints: rolling_s=" << rolling_seconds
              << " rescan_s=" << reference_seconds << " speedup=" << reference_seconds / rolling_seconds
              << " checksum_difference=" << rolling_checksum - reference_checksum << '\n';
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--benchmark") { benchmark(); return 0; }
        double max_error = 0;
        // Multiple full ring wraps, every lag, non-aligned checkpoint blocks.
        for (unsigned kind = 0; kind < 4; ++kind)
            max_error = std::max(max_error, check(fixture(600001, kind), 2053));
        // Dense checks include before/at/after the shortest readiness boundary.
        max_error = std::max(max_error, check(fixture(2300, 0), 1));
        reset_checks();
        std::cout << "Rolling repetition exact-window tests passed; max_error=" << max_error << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
