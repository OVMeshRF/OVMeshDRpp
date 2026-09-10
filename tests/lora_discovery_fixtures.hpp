// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <random>
#include <span>
#include <stdexcept>
#include <vector>

// Independent analytic sources for receive-only discovery tests. No application
// encoders, modulators, FFTs, channelizers or receiver helpers are used here.
// Waveform reference: M. Chiani and A. Elzanaty, "On the LoRa Modulation for IoT:
// Waveform Properties and Spectral Analysis" (2019), Eq. (5):
// https://arxiv.org/pdf/1906.04256
// Framing reference: J. Tapparel and A. Burg, "Design and Implementation of LoRa
// Physical Layer in GNU Radio", GNU Radio Conference 2024, section 2.9, Eq. (9):
// https://events.gnuradio.org/event/24/contributions/641/attachments/192/478/paper_tapparel.pdf
// Original test implementation of those mathematical descriptions; no source
// code copied from either reference. It models an ideal complex baseband with
// AWGN and static frequency/timing offsets. It does not model analogue filtering,
// clock drift, multipath, quantization, coding, a valid payload or a mesh protocol.
namespace lora_discovery_fixtures {

using Complex = std::complex<float>;
inline constexpr std::size_t max_samples = 16 * 1024 * 1024;

struct PreambleSpec {
    double sample_rate_hz = 2'000'000;
    double center_offset_hz = 0;
    std::uint32_t bandwidth_hz = 250'000;
    unsigned spreading_factor = 11;
    unsigned preamble_symbols = 8;
    std::uint8_t sync_word = 0x12;
    std::size_t leading_samples = 137;
    std::size_t trailing_samples = 4096;
    // Start = leading_samples + fractional_start_samples; not rounded before
    // evaluating the waveform. All truth endpoints use half-open intervals.
    double fractional_start_samples = 0;
    double cfo_hz = 0;
    double amplitude = .2;
    // sqrt(E[|noise|^2]); each independent I/Q component has variance rms^2/2.
    double noise_rms = 0;
    std::uint64_t seed = 0x164730b5e2ULL;
    double initial_phase_radians = .37;
};

struct FrameTruth {
    double sample_rate_hz = 0;
    double start_sample = 0;
    double preamble_end_sample = 0;
    double sync_end_sample = 0;
    double sfd_end_sample = 0;
    // Optional payload-like chirp tail; no encoded bytes or protocol payload.
    double chirp_tail_end_sample = 0;
    std::size_t chirp_tail_symbols = 0;
    // Discrete samples at integer indices in [first_sample, end_sample) contain
    // signal; fractional continuous-time edges are recorded above separately.
    std::size_t first_sample = 0;
    std::size_t end_sample = 0;
    double symbol_samples = 0;
    double symbol_seconds = 0;
    double configured_center_offset_hz = 0;
    double received_center_offset_hz = 0;
    double lower_offset_hz = 0;
    double upper_offset_hz = 0;
    std::uint32_t bandwidth_hz = 0;
    unsigned spreading_factor = 0;
    unsigned preamble_symbols = 0;
    std::uint8_t sync_word = 0;
};

struct Fixture {
    std::vector<Complex> samples;
    FrameTruth truth;
};

namespace detail {
inline void require(bool condition, const char* reason) {
    if (!condition) throw std::invalid_argument(reason);
}

inline void validate_rate(double rate) {
    require(std::isfinite(rate) && rate >= 125'000 && rate <= 20'000'000,
            "Fixture sample rate must be finite and within 125 kS/s to 20 MS/s");
}

inline void validate_amplitude(double amplitude) {
    require(std::isfinite(amplitude) && amplitude >= 0 && amplitude <= 4,
            "Fixture amplitude must be finite and within 0 to 4");
}

inline void validate_band(double rate, double center, double half_width) {
    validate_rate(rate);
    require(std::isfinite(center) && std::isfinite(half_width) && half_width >= 0 &&
            std::abs(center) + half_width < rate / 2,
            "Fixture instantaneous-frequency range must lie inside Nyquist");
}

// Mapping engine outputs explicitly avoids implementation-dependent
// normal_distribution algorithms. Standard floating-point math may still vary
// by last bits across platforms; tests should compare waveforms with tolerance.
inline double uniform_open(std::mt19937_64& random) {
    constexpr double denominator = 4503599627370496.; // 2^52
    return (static_cast<double>(random() >> 12) + .5) / denominator;
}

inline Complex phasor(double cycles, double amplitude) {
    const double phase = 2 * std::numbers::pi * std::remainder(cycles, 1.);
    return {static_cast<float>(amplitude * std::cos(phase)),
            static_cast<float>(amplitude * std::sin(phase))};
}

inline std::size_t checked_size(double size) {
    require(std::isfinite(size) && size >= 0 && size <= static_cast<double>(max_samples),
            "Fixture exceeds the bounded sample allocation");
    return static_cast<std::size_t>(std::ceil(size));
}

// Unit-symbol waveform integrated from its piecewise instantaneous frequency.
// The wrap changes phase by an integer number of cycles, preserving continuity.
inline double chirp_cycles(double symbol_time, double bandwidth, unsigned chips,
                           unsigned symbol) {
    const double chip_time = bandwidth * symbol_time;
    const double wrap = chip_time >= static_cast<double>(chips - symbol) ? 1. : 0.;
    return chip_time * (static_cast<double>(symbol) / chips - .5 +
                       chip_time / (2 * chips) - wrap);
}
} // namespace detail

inline void add_noise(std::span<Complex> samples, double rms, std::uint64_t seed) {
    detail::require(samples.size() <= max_samples && std::isfinite(rms) &&
                    rms >= 0 && rms <= 4,
                    "Fixture noise must be bounded and finite");
    if (rms == 0) return;
    std::mt19937_64 random(seed);
    for (auto& value : samples) {
        // Box-Muller pair, scaled by rms/sqrt(2).
        const double radius = rms * std::sqrt(-std::log(detail::uniform_open(random)));
        const double phase = 2 * std::numbers::pi * detail::uniform_open(random);
        value += Complex(static_cast<float>(radius * std::cos(phase)),
                         static_cast<float>(radius * std::sin(phase)));
    }
}

inline Fixture make_preamble(const PreambleSpec& spec) {
    detail::require(spec.bandwidth_hz == 125'000 || spec.bandwidth_hz == 250'000 ||
                    spec.bandwidth_hz == 500'000,
                    "Fixture supports 125, 250 and 500 kHz LoRa bandwidths");
    detail::require(spec.spreading_factor >= 7 && spec.spreading_factor <= 12 &&
                    spec.preamble_symbols >= 6 && spec.preamble_symbols <= 64,
                    "Fixture SF or preamble count is outside the bounded range");
    detail::require(std::isfinite(spec.fractional_start_samples) &&
                    spec.fractional_start_samples >= 0 && spec.fractional_start_samples < 1 &&
                    std::isfinite(spec.center_offset_hz) && std::isfinite(spec.cfo_hz) &&
                    std::isfinite(spec.initial_phase_radians) &&
                    spec.leading_samples <= max_samples && spec.trailing_samples <= max_samples,
                    "Fixture timing, frequency or phase is invalid");
    detail::validate_band(spec.sample_rate_hz, spec.center_offset_hz + spec.cfo_hz,
                          spec.bandwidth_hz / 2.);
    detail::validate_amplitude(spec.amplitude);
    detail::require(std::isfinite(spec.noise_rms) && spec.noise_rms >= 0 && spec.noise_rms <= 4,
                    "Fixture noise RMS must be finite and within 0 to 4");

    Fixture fixture;
    auto& truth = fixture.truth;
    const unsigned chips = 1u << spec.spreading_factor;
    truth.sample_rate_hz = spec.sample_rate_hz;
    truth.start_sample = static_cast<double>(spec.leading_samples) + spec.fractional_start_samples;
    truth.symbol_seconds = static_cast<double>(chips) / spec.bandwidth_hz;
    truth.symbol_samples = truth.symbol_seconds * spec.sample_rate_hz;
    truth.preamble_end_sample = truth.start_sample + spec.preamble_symbols * truth.symbol_samples;
    truth.sync_end_sample = truth.preamble_end_sample + 2 * truth.symbol_samples;
    truth.sfd_end_sample = truth.sync_end_sample + 2.25 * truth.symbol_samples;
    truth.chirp_tail_end_sample = truth.sfd_end_sample;
    truth.first_sample = detail::checked_size(truth.start_sample);
    truth.end_sample = detail::checked_size(truth.sfd_end_sample);
    truth.configured_center_offset_hz = spec.center_offset_hz;
    truth.received_center_offset_hz = spec.center_offset_hz + spec.cfo_hz;
    truth.lower_offset_hz = truth.received_center_offset_hz - spec.bandwidth_hz / 2.;
    truth.upper_offset_hz = truth.received_center_offset_hz + spec.bandwidth_hz / 2.;
    truth.bandwidth_hz = spec.bandwidth_hz;
    truth.spreading_factor = spec.spreading_factor;
    truth.preamble_symbols = spec.preamble_symbols;
    truth.sync_word = spec.sync_word;
    fixture.samples.resize(detail::checked_size(truth.sfd_end_sample + spec.trailing_samples));

    for (std::size_t i = truth.first_sample; i < truth.end_sample; ++i) {
        const double elapsed = (static_cast<double>(i) - truth.start_sample) / spec.sample_rate_hz;
        const double symbol_position = elapsed / truth.symbol_seconds;
        const auto symbol_index = static_cast<unsigned>(std::floor(symbol_position));
        const double within_symbol = (symbol_position - symbol_index) * truth.symbol_seconds;
        unsigned value = 0;
        if (symbol_index == spec.preamble_symbols) value = (spec.sync_word >> 4) * 8u;
        if (symbol_index == spec.preamble_symbols + 1) value = (spec.sync_word & 15u) * 8u;
        const bool down = symbol_index >= spec.preamble_symbols + 2;
        double cycles = detail::chirp_cycles(within_symbol, spec.bandwidth_hz, chips, value);
        if (down) cycles = -cycles;
        cycles += truth.received_center_offset_hz * elapsed +
                  spec.initial_phase_radians / (2 * std::numbers::pi);
        fixture.samples[i] = detail::phasor(cycles, spec.amplitude);
    }
    add_noise(fixture.samples, spec.noise_rms, spec.seed);
    return fixture;
}

// A continuous chirp-symbol tail starts immediately after the quarter downchirp,
// rather than replacing post-SFD samples with silence. Symbol indices exercise
// header/payload-like RF transitions but are not encoded packet/application data.
// The preamble generator and tail both evaluate the published analytic equation;
// neither uses application modulation, packet encoders, FFTs or receiver helpers.
inline Fixture make_preamble_with_chirp_tail(const PreambleSpec& spec,
                                            std::span<const unsigned> symbols) {
    detail::require(!symbols.empty() && symbols.size() <= 128,
                    "Chirp tail must contain 1 through 128 symbols");
    detail::require(spec.spreading_factor >= 7 && spec.spreading_factor <= 12,
                    "Chirp-tail SF is outside the supported range");
    const unsigned chips = 1u << spec.spreading_factor;
    for(unsigned symbol:symbols)
        detail::require(symbol<chips,"Chirp-tail symbol index exceeds the alphabet");
    // Generate AWGN once over the final duration, including leading and trailing
    // silence. This keeps noise statistics uniform through the SFD transition.
    PreambleSpec clean=spec;clean.noise_rms=0;clean.trailing_samples=0;
    auto fixture=make_preamble(clean);auto& truth=fixture.truth;
    truth.chirp_tail_symbols=symbols.size();
    truth.chirp_tail_end_sample=truth.sfd_end_sample+static_cast<double>(symbols.size())*truth.symbol_samples;
    const auto tail_first=detail::checked_size(truth.sfd_end_sample);
    truth.end_sample=detail::checked_size(truth.chirp_tail_end_sample);
    fixture.samples.resize(detail::checked_size(truth.chirp_tail_end_sample+spec.trailing_samples));
    // Explicitly carry the partial downchirp's endpoint phase. Complete chirps
    // have integer-cycle endpoint differences, also retained in the prefix sum.
    std::vector<double> phase_prefix(symbols.size()+1);
    phase_prefix[0]=-detail::chirp_cycles(.25*truth.symbol_seconds,spec.bandwidth_hz,chips,0);
    for(std::size_t i=0;i<symbols.size();++i)
        phase_prefix[i+1]=phase_prefix[i]+detail::chirp_cycles(truth.symbol_seconds,spec.bandwidth_hz,chips,symbols[i]);
    for(std::size_t i=tail_first;i<truth.end_sample;++i) {
        const double symbol_position=(static_cast<double>(i)-truth.sfd_end_sample)/truth.symbol_samples;
        const auto symbol_index=static_cast<std::size_t>(std::floor(symbol_position));
        detail::require(symbol_index<symbols.size(),"Unrepresentable chirp-tail boundary");
        const double within_symbol=(symbol_position-static_cast<double>(symbol_index))*truth.symbol_seconds;
        const double elapsed=(static_cast<double>(i)-truth.start_sample)/spec.sample_rate_hz;
        const double cycles=phase_prefix[symbol_index]+
            detail::chirp_cycles(within_symbol,spec.bandwidth_hz,chips,symbols[symbol_index])+
            truth.received_center_offset_hz*elapsed+spec.initial_phase_radians/(2*std::numbers::pi);
        fixture.samples[i]=detail::phasor(cycles,spec.amplitude);
    }
    add_noise(fixture.samples,spec.noise_rms,spec.seed);
    return fixture;
}

// The following sources can fill a zero vector for negative controls or be
// superimposed on a preamble. They deliberately carry no LoRa framing.
inline void add_cw(std::span<Complex> samples, double sample_rate_hz,
                   double offset_hz, double amplitude, double initial_phase_radians = .19) {
    detail::validate_band(sample_rate_hz, offset_hz, 0);
    detail::validate_amplitude(amplitude);
    detail::require(samples.size() <= max_samples && std::isfinite(initial_phase_radians),
                    "CW fixture length or phase is invalid");
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] += detail::phasor(offset_hz * static_cast<double>(i) / sample_rate_hz +
                                    initial_phase_radians / (2 * std::numbers::pi), amplitude);
}

struct FskSpec {
    double sample_rate_hz = 2'000'000;
    double center_offset_hz = 0;
    double deviation_hz = 20'000;
    double symbol_rate_hz = 10'000;
    double amplitude = .2;
    double initial_phase_radians = .23;
    // Rectangular, continuous-phase binary FSK with an explicit repeating bit
    // pattern. This is a generic negative control, not a particular ISM device.
    std::vector<std::uint8_t> pattern{0, 1, 1, 0, 1, 0, 0, 1};
};

inline void add_fsk(std::span<Complex> samples, const FskSpec& spec) {
    detail::validate_band(spec.sample_rate_hz, spec.center_offset_hz, spec.deviation_hz);
    detail::validate_amplitude(spec.amplitude);
    detail::require(samples.size() <= max_samples && !spec.pattern.empty() &&
                    spec.pattern.size() <= 4096 && std::isfinite(spec.symbol_rate_hz) &&
                    spec.symbol_rate_hz > 0 && spec.symbol_rate_hz <= spec.sample_rate_hz / 2 &&
                    std::isfinite(spec.initial_phase_radians),
                    "FSK fixture length, bit pattern, symbol rate or phase is invalid");
    std::vector<int> prefix(spec.pattern.size() + 1);
    for (std::size_t i = 0; i < spec.pattern.size(); ++i) {
        detail::require(spec.pattern[i] <= 1, "FSK fixture pattern contains a non-binary value");
        prefix[i + 1] = prefix[i] + (spec.pattern[i] ? 1 : -1);
    }
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double seconds = static_cast<double>(i) / spec.sample_rate_hz;
        const double bit_position = seconds * spec.symbol_rate_hz;
        const auto bit = static_cast<std::uint64_t>(std::floor(bit_position));
        const auto cycles = bit / spec.pattern.size();
        const auto position = static_cast<std::size_t>(bit % spec.pattern.size());
        const double integrated_bits = static_cast<double>(cycles) * prefix.back() + prefix[position] +
            (bit_position - static_cast<double>(bit)) * (spec.pattern[position] ? 1 : -1);
        const double phase_cycles = spec.center_offset_hz * seconds +
            spec.deviation_hz * integrated_bits / spec.symbol_rate_hz +
            spec.initial_phase_radians / (2 * std::numbers::pi);
        samples[i] += detail::phasor(phase_cycles, spec.amplitude);
    }
}

} // namespace lora_discovery_fixtures
