// SPDX-License-Identifier: GPL-3.0-or-later
#include "lora_discovery_fixtures.hpp"

#include <array>
#include <iostream>
#include <string>

namespace {
using namespace lora_discovery_fixtures;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void near(double actual, double expected, double tolerance, const std::string& message) {
    require(std::abs(actual - expected) <= tolerance,
            message + ": actual=" + std::to_string(actual) + ", expected=" + std::to_string(expected));
}

void analytic_waveform_and_two_tone_model() {
    unsigned cases = 0;
    for (auto bandwidth : {125'000u, 250'000u, 500'000u}) {
        for (unsigned sf = 7; sf <= 12; ++sf) {
            PreambleSpec spec;
            spec.bandwidth_hz = bandwidth;
            spec.spreading_factor = sf;
            spec.center_offset_hz = sf % 2 ? 377'133. : -341'789.;
            spec.cfo_hz = -913.25;
            spec.sync_word = sf % 2 ? 0x2b : 0x34;
            spec.fractional_start_samples = .37;
            const auto fixture = make_preamble(spec);
            const auto& truth = fixture.truth;
            const double rate = spec.sample_rate_hz;
            const double bw = bandwidth;
            const double symbol_samples = rate * static_cast<double>(1u << sf) / bw;
            near(truth.symbol_samples, symbol_samples, 1e-9, "Independent symbol duration");
            near(truth.sfd_end_sample - truth.start_sample, 12.25 * symbol_samples, 1e-9,
                 "Independent preamble/framing duration");
            require(fixture.samples[truth.first_sample - 1] == Complex{} &&
                    fixture.samples[truth.end_sample] == Complex{}, "Half-open finite signal interval");

            // Independent instantaneous-frequency oracle from the linear ramp,
            // compared through adjacent IQ phase. Avoid crossing a symbol edge
            // here; phase continuity at a wrap is covered by the two-tone fit.
            for (unsigned symbol = 0; symbol < spec.preamble_symbols + 4; ++symbol) {
                const unsigned sync_value = symbol == spec.preamble_symbols ? (spec.sync_word >> 4) * 8u :
                    symbol == spec.preamble_symbols + 1 ? (spec.sync_word & 15u) * 8u : 0;
                const double symbol_start = truth.start_sample + symbol * symbol_samples;
                for (double fraction : {.03, .17, .43, .71, .93}) {
                    const auto n = static_cast<std::size_t>(std::ceil(symbol_start + fraction * symbol_samples));
                    const double local_midpoint = (static_cast<double>(n) + .5 - symbol_start) / rate;
                    const double symbol_seconds = symbol_samples / rate;
                    const double wrap_seconds = (static_cast<double>(1u << sf) - sync_value) / bw;
                    if (local_midpoint + .5 / rate >= symbol_seconds ||
                        std::abs(local_midpoint - wrap_seconds) <= 1 / rate) continue;
                    double expected_hz = -bw / 2 + bw * local_midpoint / symbol_seconds +
                        sync_value * bw / (1u << sf);
                    if (local_midpoint >= wrap_seconds) expected_hz -= bw;
                    if (symbol >= spec.preamble_symbols + 2) expected_hz = -expected_hz;
                    expected_hz += spec.center_offset_hz + spec.cfo_hz;
                    const auto adjacent = std::complex<double>(fixture.samples[n + 1]) *
                                          std::conj(std::complex<double>(fixture.samples[n]));
                    const double observed_hz = std::arg(adjacent) * rate / (2 * std::numbers::pi);
                    near(observed_hz, expected_hz, .04, "Independent chirp phase derivative");
                }
            }

            // A window starting at an arbitrary location within repeated
            // preamble contains a high then a low tone after dechirping. The
            // tones differ by exactly BW and their phases agree at the wrap.
            const auto window = static_cast<std::size_t>(truth.start_sample + 1.314159 * symbol_samples);
            const auto count = static_cast<std::size_t>(symbol_samples);
            const double wrap_sample = truth.start_sample + 2 * symbol_samples - static_cast<double>(window);
            const double center = spec.center_offset_hz + spec.cfo_hz;
            const double high = center + bw * (1 - wrap_sample / symbol_samples);
            const double low = high - bw;
            std::complex<double> coherent{};
            double energy = 0;
            for (std::size_t j = 0; j < count; ++j) {
                const double seconds = static_cast<double>(j) / rate;
                // Integrate the reference linear frequency ramp directly,
                // rather than calling the fixture's chirp_cycles implementation.
                const double reference_cycles = -bw * seconds / 2 +
                    bw * seconds * seconds / (2 * truth.symbol_seconds);
                const double tone_cycles = static_cast<double>(j) < wrap_sample ? high * seconds :
                    low * seconds + bw * wrap_sample / rate;
                const auto value = std::complex<double>(fixture.samples[window + j]);
                coherent += value * std::polar(1., -2 * std::numbers::pi * (reference_cycles + tone_cycles));
                energy += std::norm(value);
            }
            const double coherence = std::norm(coherent) / (static_cast<double>(count) * energy);
            near(coherence, 1., 1e-10, "Piecewise-tone phase continuity across chirp reset");
            ++cases;
        }
    }
    std::cout << cases << " independent BW/SF phase and framing checks passed\n";
}

void arbitrary_sampling_and_noise() {
    PreambleSpec spec;
    spec.sample_rate_hz = 1'999'983;
    spec.spreading_factor = 8;
    spec.bandwidth_hz = 500'000;
    spec.fractional_start_samples = .618;
    const auto fixture = make_preamble(spec);
    near(fixture.truth.symbol_samples, 1'999'983. * 256 / 500'000, 1e-12,
         "Symbol length need not be an integer number of samples");
    require(fixture.truth.first_sample == spec.leading_samples + 1,
            "Fractional timing does not round down signal onset");
    for (std::size_t i = fixture.truth.first_sample; i < fixture.truth.end_sample; ++i)
        near(std::abs(fixture.samples[i]), spec.amplitude, 2e-8, "Ideal chirp constant envelope");

    std::vector<Complex> noise(131072), repeated(noise.size());
    add_noise(noise, .1, 4827);
    add_noise(repeated, .1, 4827);
    require(noise == repeated, "Noise sequence repeats with the same seed");
    double energy = 0;
    std::complex<double> mean{};
    for (auto value : noise) { energy += std::norm(std::complex<double>(value)); mean += value; }
    near(energy / noise.size(), .01, .0001, "Complex noise RMS definition");
    require(std::abs(mean / static_cast<double>(noise.size())) < .001,
            "Noise components have near-zero mean");
}

void negative_controls_and_bounds() {
    std::vector<Complex> cw(8192);
    add_cw(cw, 2'000'000, 231'981, .2);
    for (std::size_t i = 0; i + 1 < cw.size(); ++i) {
        const auto adjacent = std::complex<double>(cw[i + 1]) * std::conj(std::complex<double>(cw[i]));
        near(std::arg(adjacent) * 2'000'000 / (2 * std::numbers::pi), 231'981, .04,
             "CW constant frequency");
    }
    std::vector<Complex> fsk(8192);
    FskSpec spec;
    spec.center_offset_hz = -150'000;
    spec.symbol_rate_hz = 17'003;
    add_fsk(fsk, spec);
    for (std::size_t i = 0; i + 1 < fsk.size(); ++i) {
        const double start_bit = static_cast<double>(i) * spec.symbol_rate_hz / spec.sample_rate_hz;
        const double end_bit = static_cast<double>(i + 1) * spec.symbol_rate_hz / spec.sample_rate_hz;
        if (std::floor(start_bit) != std::floor(end_bit)) continue;
        const auto bit = static_cast<std::size_t>(start_bit) % spec.pattern.size();
        const double expected = spec.center_offset_hz + (spec.pattern[bit] ? 1 : -1) * spec.deviation_hz;
        const auto adjacent = std::complex<double>(fsk[i + 1]) * std::conj(std::complex<double>(fsk[i]));
        near(std::arg(adjacent) * spec.sample_rate_hz / (2 * std::numbers::pi), expected, .04,
             "Rectangular FSK known instantaneous frequency");
    }
    unsigned rejected = 0;
    for (unsigned kind = 0; kind < 4; ++kind) {
        PreambleSpec invalid;
        if (kind == 0) invalid.bandwidth_hz = 200'000;
        if (kind == 1) invalid.fractional_start_samples = 1;
        if (kind == 2) invalid.leading_samples = max_samples;
        if (kind == 3) invalid.center_offset_hz = invalid.sample_rate_hz / 2;
        try { (void)make_preamble(invalid); }
        catch (const std::invalid_argument&) { ++rejected; }
    }
    require(rejected == 4, "Invalid or excessive fixtures fail before allocation");
}
} // namespace

int main() {
    try {
        analytic_waveform_and_two_tone_model();
        arbitrary_sampling_and_noise();
        negative_controls_and_bounds();
        std::cout << "Discovery fixture equation self-checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
