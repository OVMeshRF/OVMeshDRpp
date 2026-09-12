// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "discovery_iq_wipe.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace ovmesh {

// Experimental rolling replacement for the four-lag differential chirp screen.
// No detection/identity claim follows from this inexpensive screen. Global
// every-fourth-sample selection matches the original at N/4 checkpoints.
// Shared DC/energy statistics cover nine symbol periods. Correlations cover
// 62.5/125/250/500 kHz SF7-12 and 15.625 kHz SF7-10. Storage is fixed.
// Near cancellation, potentially passing decisions use the original direct
// sum to avoid changing its energy/DC subtraction boundary through roundoff.
class DiscoveryChirpScreen {
    using C = std::complex<float>;
    using D = std::complex<double>;
public:
    static constexpr std::size_t capacity = 262144, lag_count = 4;
    struct Statistic {
        D correlation{}, dc{};
        double energy = 0, residual = 0;
        std::size_t count = 0;
        bool available = false, recomputed = false;
        bool passes() const noexcept {
            return available && residual > energy * 1e-12 && std::norm(correlation) >= 8 * residual;
        }
    };

    DiscoveryChirpScreen() = default;
    DiscoveryChirpScreen(const DiscoveryChirpScreen&) = delete;
    DiscoveryChirpScreen& operator=(const DiscoveryChirpScreen&) = delete;

    void reset() noexcept {
        if (initialized_) {
            // Only global indices divisible by four are stored. Round each
            // exclusive endpoint up without overflowing near UINT64_MAX.
            const auto first = begin_ / 4 + static_cast<bool>(begin_ % 4);
            const auto end = end_ / 4 + static_cast<bool>(end_ % 4);
            discovery_detail::wipe_ring(history_, first, end - first);
        }
        common_ = {}; cross_ = {};
        begin_ = end_ = 0; initialized_ = false; recomputations_ = 0;
    }

    void push(C value, std::uint64_t index) {
        // All samples still require validation, including those not used by
        // the screen. Finite float components cannot overflow a double norm.
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag()) ||
            index == std::numeric_limits<std::uint64_t>::max() ||
            (initialized_ && index != end_)) {
            reset();
            throw std::invalid_argument("Invalid differential-screen sample or discontinuity; history reset");
        }
        if (!initialized_) { begin_ = end_ = index; initialized_ = true; }
        if (!(index & 3)) {
            update(D(value), index);
            history_[(index >> 2) & (history_.size() - 1)] = value;
        }
        end_ = index + 1;
    }

    Statistic statistic(std::uint32_t bandwidth, unsigned sf, std::size_t lag) const {
        const auto b = bandwidth_index(bandwidth);
        validate_sf_lag(sf, lag);
        if (b == 0 && sf > 10) throw std::invalid_argument("Tiny discovery supports SF7 through SF10");
        const auto period = static_cast<std::size_t>(sf - 2) - b;
        const std::size_t n = std::size_t{512} << period;
        Statistic result; result.count = n / 4;
        if (!ready(n, n / (8u << lag))) return result;
        const auto& accumulated = common_[period][lag];
        const auto& reference = references()[b][lag];
        // For SF7's fourth lag, N/4 is only half a reference period. Rotate
        // back to the original local-window phase when returning the complex
        // statistic; the pass decision uses phase-invariant squared magnitude.
        const auto rotation = std::conj(D(reference[(end_ - n) & (reference.size() - 1)]));
        result.correlation = cross_[b][sf - 7][lag] * rotation;
        result.dc = accumulated.dc;
        result.energy = std::max(0., accumulated.energy);
        result.residual = std::max(0., result.energy - std::norm(result.dc) / static_cast<double>(result.count));
        result.available = true;
        if (needs_recompute(result.energy, result.residual, std::norm(result.correlation)))
            return recompute(b, sf, lag);
        return result;
    }

    bool passes(std::uint32_t bandwidth, unsigned sf) const {
        const auto b = bandwidth_index(bandwidth);
        validate_sf_lag(sf, 0);
        if (b == 0 && sf > 10) throw std::invalid_argument("Tiny discovery supports SF7 through SF10");
        const auto period = static_cast<std::size_t>(sf - 2) - b;
        const std::size_t n = std::size_t{512} << period;
        for (std::size_t lag = 0; lag < lag_count; ++lag) {
            if (!ready(n, n / (8u << lag))) continue;
            const auto& accumulated = common_[period][lag];
            const double energy = std::max(0., accumulated.energy);
            const double residual = std::max(0., energy - std::norm(accumulated.dc) / static_cast<double>(n / 4));
            const double correlated = std::norm(cross_[b][sf - 7][lag]);
            if (needs_recompute(energy, residual, correlated)) {
                if (recompute(b, sf, lag).passes()) return true;
            } else if (residual > energy * 1e-12 && correlated >= 8 * residual) return true;
        }
        return false;
    }

    std::uint64_t recomputation_count() const noexcept { return recomputations_; }

private:
    struct Common { D dc{}; double energy = 0; };
    using References = std::array<std::array<std::vector<C>, lag_count>, 6>;
    static const References& references() {
        static const auto table = [] {
            References result;
            for (std::size_t b = 0; b < 6; ++b)
                for (std::size_t lag = 0; lag < lag_count; ++lag) {
                    auto& row = result[b][lag];
                    row.resize((std::size_t{1024} << lag) >> b);
                    for (std::size_t i = 0; i < row.size(); ++i) {
                        const double angle = -2 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(row.size());
                        row[i] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
                    }
                }
            return result;
        }();
        return table;
    }
    static std::size_t bandwidth_index(std::uint32_t bandwidth) {
        if (bandwidth == 15625) return 0;
        if (bandwidth == 62500) return 2;
        if (bandwidth == 125000) return 3;
        if (bandwidth == 250000) return 4;
        if (bandwidth == 500000) return 5;
        throw std::invalid_argument("Unsupported differential-screen bandwidth");
    }
    static void validate_sf_lag(unsigned sf, std::size_t lag) {
        if (sf < 7 || sf > 12 || lag >= lag_count)
            throw std::invalid_argument("Unsupported differential-screen SF or lag");
    }
    bool ready(std::size_t n, std::size_t delay) const noexcept {
        return initialized_ && end_ - begin_ >= n + delay && !(end_ % (n / 4));
    }
    // Every caller uses a global index divisible by four: the update cadence,
    // all delays, and all checkpoint/local-window offsets are multiples of four.
    D sample(std::uint64_t index) const noexcept { return D(history_[(index >> 2) & (history_.size() - 1)]); }

    static bool needs_recompute(double energy, double residual, double correlated) noexcept {
        // A passing original decision requires correlation >= 8*energy*1e-12.
        // Use an eightfold guard margin before skipping a cancellation rescan.
        // Pure CW has negligible non-DC correlation, so does not rescan.
        return energy > 0 && residual <= energy * 1e-8 && correlated >= energy * 1e-12;
    }
    Statistic recompute(std::size_t b, unsigned sf, std::size_t lag) const {
        if (recomputations_ < std::numeric_limits<std::uint64_t>::max()) ++recomputations_;
        const std::size_t n = std::size_t{512} << (static_cast<std::size_t>(sf - 2) - b);
        const auto delay = n / (8u << lag);
        const auto& reference = references()[b][lag];
        Statistic result; result.count = n / 4; result.available = result.recomputed = true;
        for (std::size_t i = 0; i < n; i += 4) {
            const auto z = sample(end_ - n + i) * std::conj(sample(end_ - n - delay + i));
            result.correlation += z * D(reference[i & (reference.size() - 1)]);
            result.dc += z; result.energy += std::norm(z);
        }
        result.residual = std::max(0., result.energy - std::norm(result.dc) / static_cast<double>(result.count));
        return result;
    }

    void update(D value, std::uint64_t index) {
        std::array<D, 12> new_products{};
        std::array<double, 12> new_energy{};
        for (std::size_t i = 0; i < new_products.size(); ++i) {
            new_products[i] = value * std::conj(sample(index - (std::uint64_t{8} << i)));
            new_energy[i] = std::norm(new_products[i]);
        }
        std::array<std::array<D, lag_count>, 9> deltas{};
        for (std::size_t p = 0; p < common_.size(); ++p) {
            const std::uint64_t n = std::uint64_t{512} << p;
            const auto previous = sample(index - n);
            for (std::size_t lag = 0; lag < lag_count; ++lag) {
                const auto delay_index = p + 3 - lag;
                const auto old = previous * std::conj(sample(index - n - (n / (8u << lag))));
                const auto delta = new_products[delay_index] - old;
                deltas[p][lag] = delta;
                common_[p][lag].dc += delta;
                common_[p][lag].energy += new_energy[delay_index] - std::norm(old);
            }
        }
        const auto& reference = references();
        for (const auto b : std::array<std::size_t, 5>{0, 2, 3, 4, 5})
            for (std::size_t lag = 0; lag < lag_count; ++lag) {
                const auto& row = reference[b][lag];
                const D coefficient(row[index & (row.size() - 1)]);
                const auto sf_count = b == 0 ? std::size_t{4} : cross_[b].size();
                for (std::size_t s = 0; s < sf_count; ++s)
                    cross_[b][s][lag] += deltas[s + 5 - b][lag] * coefficient;
            }
    }

    // The logical history/cadence is unchanged; unused intermediate samples
    // are never retained. This is storage compaction, not additional decimation.
    std::array<C, capacity / 4> history_{};
    std::array<std::array<Common, lag_count>, 9> common_{};
    std::array<std::array<std::array<D, lag_count>, 6>, 6> cross_{};
    std::uint64_t begin_ = 0, end_ = 0;
    mutable std::uint64_t recomputations_ = 0;
    bool initialized_ = false;
};
}
