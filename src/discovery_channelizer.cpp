// SPDX-License-Identifier: GPL-3.0-or-later
#include "discovery_channelizer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace ovmesh {
namespace {
using Complex = std::complex<float>;
constexpr double pi = std::numbers::pi;

// A small bounded I0 series avoids a library or platform-dependent Bessel API.
double bessel_i0(double x) {
    double result = 1, term = 1;
    const double square = x * x / 4;
    for (unsigned n = 1; n < 64; ++n) {
        term *= square / (static_cast<double>(n) * n);
        result += term;
        if (term < result * 1e-16) break;
    }
    return result;
}
} // namespace

class DiscoveryChannelizer::Impl {
public:
    uint32_t rate, phases, stride, delay;
    std::vector<Subband> descriptions;
    std::vector<size_t> bins;
    std::vector<float> taps;
    std::vector<Complex> history, polyphase, transformed, roots;
    std::array<std::vector<Complex>, 2> rotations;
    std::vector<size_t> reversal;
    std::vector<std::vector<Complex>> outputs;
    size_t history_position = 0, filled = 0, decimation_phase = 0;
    uint64_t next_input = 0, origin = 0, output_first = 0;
    bool initialized = false, modulation_phase = false;

    Impl(uint32_t sample_rate, double center, double lower, double upper)
        : rate(sample_rate), phases(sample_rate / center_spacing_hz),
          stride(sample_rate / output_sample_rate),
          delay(static_cast<uint32_t>(phases * taps_per_phase / 2)) {
        constexpr std::array<uint32_t, 5> supported{8000000, 10000000, 12000000,
                                                   16000000, 20000000};
        if (std::find(supported.begin(), supported.end(), rate) == supported.end())
            throw std::invalid_argument("Discovery channelizer requires 8, 10, 12, 16 or 20 MS/s");
        if (!std::isfinite(center) || !std::isfinite(lower) || !std::isfinite(upper) ||
            lower < 0 || lower >= upper || lower < center - rate / 2.0 ||
            upper > center + rate / 2.0)
            throw std::invalid_argument("Invalid discovery frequency range");

        // Exclude the Nyquist-wrapped channel. Every advertised passband is
        // wholly inside the supplied complex-IQ Nyquist interval.
        for (int k = -static_cast<int>(phases / 2) + 1;
             k < static_cast<int>(phases / 2); ++k) {
            const double frequency = center + k * static_cast<double>(center_spacing_hz);
            const double low = frequency - passband_half_width_hz;
            const double high = frequency + passband_half_width_hz;
            if (high <= lower || low >= upper) continue;
            descriptions.push_back({frequency, low, high});
            bins.push_back(static_cast<size_t>((k + static_cast<int>(phases)) % static_cast<int>(phases)));
        }
        if (descriptions.empty() || lower < descriptions.front().passband_lower_hz ||
            upper > descriptions.back().passband_upper_hz)
            throw std::invalid_argument("Discovery range exceeds guarded channelizer coverage");

        const size_t length = phases * taps_per_phase + 1;
        taps.resize(length);
        // Mirror a backwards-written delay line so every FIR dot product reads
        // one contiguous newest-to-oldest span, with no wrap branch per tap.
        history.resize(length * 2);
        polyphase.resize(phases);
        transformed.resize(phases);
        roots.resize(phases);
        reversal.resize(phases);
        outputs.resize(descriptions.size());
        for (auto& output : outputs) output.reserve(4096 / stride + 1);

        constexpr double beta = 7.5, cutoff_hz = 875000;
        const double cutoff = cutoff_hz / rate;
        const double window_normalization = bessel_i0(beta);
        double total = 0;
        for (size_t n = 0; n < length; ++n) {
            const double t = static_cast<double>(n) - delay;
            const double x = t / delay;
            const double window = bessel_i0(beta * std::sqrt(std::max(0.0, 1 - x * x))) /
                                  window_normalization;
            const double sinc = t == 0 ? 2 * cutoff : std::sin(2 * pi * cutoff * t) / (pi * t);
            taps[n] = static_cast<float>(sinc * window);
            total += taps[n];
        }
        for (auto& tap : taps) tap = static_cast<float>(tap / total);
        for (size_t k = 0; k < phases; ++k)
            roots[k] = std::polar(1.0f, static_cast<float>(2 * pi * k / phases));
        for (size_t p = 0; p < rotations.size(); ++p)
            for (auto bin : bins)
                rotations[p].push_back(std::conj(roots[bin * ((p + 1) * stride - 1) % phases]));
        if (std::has_single_bit(phases)) {
            const unsigned bits = static_cast<unsigned>(std::countr_zero(phases));
            for (size_t k = 0; k < phases; ++k) {
                size_t value = k, reversed = 0;
                for (unsigned bit = 0; bit < bits; ++bit) {
                    reversed = (reversed << 1) | (value & 1);
                    value >>= 1;
                }
                reversal[k] = reversed;
            }
        }
    }

    ~Impl() { reset(); }

    void reset() {
        std::fill(history.begin(), history.end(), Complex{});
        std::fill(polyphase.begin(), polyphase.end(), Complex{});
        std::fill(transformed.begin(), transformed.end(), Complex{});
        for (auto& output : outputs) {
            std::fill(output.begin(), output.end(), Complex{});
            output.clear();
        }
        history_position = filled = decimation_phase = 0;
        next_input = origin = output_first = 0;
        initialized = modulation_phase = false;
    }

    void transform() {
        if (std::has_single_bit(phases)) {
            for (size_t k = 0; k < phases; ++k) transformed[reversal[k]] = polyphase[k];
            for (size_t width = 2; width <= phases; width *= 2) {
                const auto root_stride = phases / width;
                for (size_t base = 0; base < phases; base += width) {
                    for (size_t k = 0; k < width / 2; ++k) {
                        const auto a = transformed[base + k];
                        const auto b = transformed[base + k + width / 2] * roots[k * root_stride];
                        transformed[base + k] = a + b;
                        transformed[base + k + width / 2] = a - b;
                    }
                }
            }
        } else {
            // All phases share their FIR work; the small non-power-of-two DFT
            // uses precomputed roots. Throughput must be benchmarked per rate.
            for (size_t k = 0; k < phases; ++k) {
                Complex sum{};
                for (size_t p = 0; p < phases; ++p) sum += polyphase[p] * roots[k * p % phases];
                transformed[k] = sum;
            }
        }
    }

    void sample(Complex value, uint64_t absolute) {
        history[history_position] = value;
        history[history_position + taps.size()] = value;
        if (history_position == 0) history_position = taps.size();
        --history_position;
        filled = std::min(filled + 1, taps.size());
        if (++decimation_phase < stride) return;
        decimation_phase = 0;
        const size_t modulation = modulation_phase ? 1 : 0;
        modulation_phase = !modulation_phase;
        if (filled < taps.size()) return;

        std::fill(polyphase.begin(), polyphase.end(), Complex{});
        const auto* samples = history.data() + history_position + 1;
        // Adjacent polyphase accumulators are independent. This layout allows
        // vectorization of real-tap/complex-sample arithmetic on each platform.
        for (size_t q = 0; q < taps_per_phase; ++q)
            for (size_t p = 0; p < phases; ++p)
                polyphase[p] += samples[q * phases + p] * taps[q * phases + p];
        polyphase[0] += samples[taps.size() - 1] * taps.back();
        transform();
        if (outputs.front().empty()) output_first = absolute - delay;
        // Remove the hop-dependent analysis modulation. Omitting this causes
        // every other output sample of odd channels to change sign (a 1 MHz
        // frequency error). Coordinates are relative to the segment origin.
        for (size_t i = 0; i < outputs.size(); ++i)
            outputs[i].push_back(transformed[bins[i]] * rotations[modulation][i]);
    }

    void flush(const Callback& callback) {
        for (size_t i = 0; i < outputs.size(); ++i) {
            if (!outputs[i].empty() && callback)
                callback(Samples{i, output_first, stride, outputs[i]});
            std::fill(outputs[i].begin(), outputs[i].end(), Complex{});
            outputs[i].clear();
        }
    }
};

DiscoveryChannelizer::DiscoveryChannelizer(uint32_t rate, double center, double lower, double upper)
    : impl_(std::make_unique<Impl>(rate, center, lower, upper)) {}
DiscoveryChannelizer::~DiscoveryChannelizer() = default;
DiscoveryChannelizer::DiscoveryChannelizer(DiscoveryChannelizer&&) noexcept = default;
DiscoveryChannelizer& DiscoveryChannelizer::operator=(DiscoveryChannelizer&&) noexcept = default;
const std::vector<DiscoveryChannelizer::Subband>& DiscoveryChannelizer::subbands() const noexcept { return impl_->descriptions; }
uint32_t DiscoveryChannelizer::input_sample_rate() const noexcept { return impl_->rate; }
uint32_t DiscoveryChannelizer::group_delay_input_samples() const noexcept { return impl_->delay; }
size_t DiscoveryChannelizer::prototype_tap_count() const noexcept { return impl_->taps.size(); }
void DiscoveryChannelizer::reset() { impl_->reset(); }

void DiscoveryChannelizer::feed(std::span<const Complex> input, uint64_t first, const Callback& callback) {
    if (input.size() > std::numeric_limits<uint64_t>::max() - first)
        throw std::overflow_error("Discovery input sample coordinate overflow");
    if (impl_->initialized && first != impl_->next_input)
        throw std::invalid_argument("Reset discovery channelizer before an input discontinuity");
    if (!impl_->initialized) {
        impl_->origin = first;
        impl_->next_input = first;
        impl_->initialized = true;
    }
    // Bound callback storage independently of the size of the caller's input.
    while (!input.empty()) {
        const size_t count = std::min<size_t>(input.size(), 4096);
        for (auto value : input.first(count)) {
            if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
                impl_->reset();
                throw std::invalid_argument("Non-finite discovery input; channelizer reset");
            }
            impl_->sample(value, impl_->next_input++);
        }
        input = input.subspan(count);
        impl_->flush(callback);
    }
}

} // namespace ovmesh
