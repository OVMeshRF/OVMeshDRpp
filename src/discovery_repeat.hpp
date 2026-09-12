// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "discovery_iq_wipe.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace ovmesh {

namespace discovery_detail {

// The detector already retains these samples for delimiter verification.
// Repetition is confidence metadata, so calculate it only for an up-tone that
// survives the chirp screen and FFT search. Match the rolling implementation's
// float products/powers and double accumulation without maintaining nine sums
// on every incoming sample. Bounds refer to the current uninterrupted segment.
inline double repetition_coherence(std::span<const std::complex<float>> ring,
                                   std::uint64_t begin, std::uint64_t end,
                                   std::size_t lag) {
    if (lag < 512 || lag > 131072 || !std::has_single_bit(lag))
        throw std::invalid_argument("Repetition lag must be a power of two from 512 to 131072");
    if (end < begin || end - begin < 2 * lag || ring.size() < 2 * lag) return 0;
    std::complex<double> cross{};
    double recent_power = 0, previous_power = 0;
    auto recent = static_cast<std::size_t>((end - lag) % ring.size());
    auto previous = static_cast<std::size_t>((end - 2 * lag) % ring.size());
    for (std::size_t i = 0; i < lag; ++i) {
        const auto current_sample = ring[recent], previous_sample = ring[previous];
        cross += std::complex<double>(current_sample * std::conj(previous_sample));
        recent_power += std::norm(current_sample);
        previous_power += std::norm(previous_sample);
        if (++recent == ring.size()) recent = 0;
        if (++previous == ring.size()) previous = 0;
    }
    const double denominator = recent_power * previous_power;
    if (denominator <= 1e-30) return 0;
    return std::sqrt(std::max(0., std::norm(cross) / denominator -
                                 1. / static_cast<double>(lag)));
}

} // namespace discovery_detail

// Rolling, exact-length repetition windows for the nine symbol periods used
// by the 2 MS/s discovery experiment. For lag N, compare the most recent N
// samples with the preceding N samples; no decimation or approximate IIR window
// is used. Float products reproduce the existing detector's sample arithmetic;
// double accumulators limit cancellation drift across an ongoing stream.
//
// Memory is fixed (262144 complex floats plus nine accumulator sets). push()
// rejects non-finite samples or float-power overflow and clears all history.
// Before 2*N samples are available, coherence(N) is explicitly unavailable (0).
// Reset at every source discontinuity; this class has no sample-clock semantics.
class DiscoveryRepeat {
    using Complex = std::complex<float>;
public:
    static constexpr size_t min_lag = 512;
    static constexpr size_t max_lag = 131072;
    static constexpr size_t lag_count = 9;
    static constexpr size_t capacity = 2 * max_lag;

    DiscoveryRepeat() = default;
    DiscoveryRepeat(const DiscoveryRepeat&) = delete;
    DiscoveryRepeat& operator=(const DiscoveryRepeat&) = delete;

    void reset() noexcept {
        discovery_detail::wipe_ring(history_, (position_ + capacity - filled_) & (capacity - 1), filled_);
        cross_ = {};
        recent_power_ = {};
        previous_power_ = {};
        position_ = filled_ = 0;
    }

    void push(Complex sample) {
        const double power = std::norm(sample);
        if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag()) ||
            !std::isfinite(power)) {
            reset();
            throw std::invalid_argument("Non-finite repetition-detector input; history reset");
        }

        // Delays are nested powers of two. Reuse each delayed sample and its
        // power as the next lag's recent-window endpoint. Read the oldest slot
        // before replacing it with the new sample (critical at the longest lag).
        size_t lag = min_lag;
        Complex delayed = history_[(position_ + capacity - lag) & (capacity - 1)];
        double delayed_power = std::norm(delayed);
        for (size_t i = 0; i < lag_count; ++i, lag *= 2) {
            const Complex previous = history_[(position_ + capacity - 2 * lag) & (capacity - 1)];
            const double previous_power = std::norm(previous);
            cross_[i] += std::complex<double>(sample * std::conj(delayed)) -
                         std::complex<double>(delayed * std::conj(previous));
            recent_power_[i] += power - delayed_power;
            previous_power_[i] += delayed_power - previous_power;
            delayed = previous;
            delayed_power = previous_power;
        }
        history_[position_] = sample;
        position_ = (position_ + 1) & (capacity - 1);
        filled_ = std::min(filled_ + 1, capacity);
    }

    [[nodiscard]] double coherence(size_t lag) const {
        if (lag < min_lag || lag > max_lag || !std::has_single_bit(lag))
            throw std::invalid_argument("Repetition lag must be a power of two from 512 to 131072");
        if (filled_ < 2 * lag) return 0;
        const size_t i = static_cast<size_t>(std::countr_zero(lag) - std::countr_zero(min_lag));
        const double denominator = recent_power_[i] * previous_power_[i];
        if (denominator <= 1e-30) return 0;
        return std::sqrt(std::max(0., std::norm(cross_[i]) / denominator -
                                     1. / static_cast<double>(lag)));
    }

private:
    std::array<Complex, capacity> history_{};
    std::array<std::complex<double>, lag_count> cross_{};
    std::array<double, lag_count> recent_power_{}, previous_power_{};
    size_t position_ = 0, filled_ = 0;
};

} // namespace ovmesh
