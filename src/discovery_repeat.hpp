// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <stdexcept>

namespace ovmesh {

// Rolling, exact-length repetition windows for the eight symbol periods used
// by the 2 MS/s discovery experiment. For lag N, compare the most recent N
// samples with the preceding N samples; no decimation or approximate IIR window
// is used. Float products reproduce the existing detector's sample arithmetic;
// double accumulators limit cancellation drift across an ongoing stream.
//
// Memory is fixed (131072 complex floats plus eight accumulator sets). push()
// rejects non-finite samples or float-power overflow and clears all history.
// Before 2*N samples are available, coherence(N) is explicitly unavailable (0).
// Reset at every source discontinuity; this class has no sample-clock semantics.
class DiscoveryRepeat {
    using Complex = std::complex<float>;
public:
    static constexpr size_t min_lag = 512;
    static constexpr size_t max_lag = 65536;
    static constexpr size_t lag_count = 8;
    static constexpr size_t capacity = 2 * max_lag;

    DiscoveryRepeat() = default;
    DiscoveryRepeat(const DiscoveryRepeat&) = delete;
    DiscoveryRepeat& operator=(const DiscoveryRepeat&) = delete;

    void reset() noexcept {
        std::fill(history_.begin(), history_.end(), Complex{});
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
        // before replacing it with the new sample (critical at lag 65536).
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
            throw std::invalid_argument("Repetition lag must be a power of two from 512 to 65536");
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
