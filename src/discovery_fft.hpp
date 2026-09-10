// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ovmesh {

// Experiment-only unnormalized forward radix-2 FFT. The reusable plan owns
// indices and twiddles only; execute neither allocates nor retains input IQ.
// Separate input spans may use the same immutable plan concurrently.
class DiscoveryFftPlan {
public:
    static constexpr std::size_t maximum_size = 131072;

    explicit DiscoveryFftPlan(std::size_t size) : size_(size) {
        if (size < 2 || size > maximum_size || !std::has_single_bit(size))
            throw std::invalid_argument("Discovery FFT length must be a power of two from 2 through 131072");
        permutation_.resize(size);
        for (std::size_t i = 1; i < size; ++i)
            permutation_[i] = (permutation_[i / 2] >> 1) |
                static_cast<std::uint32_t>((i & 1) ? size / 2 : 0);
        twiddles_.resize(size / 2);
        for (std::size_t i = 0; i < twiddles_.size(); ++i) {
            const double angle = -2 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(size);
            twiddles_[i] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
        }
    }

    std::size_t size() const noexcept { return size_; }

    void execute(std::span<std::complex<float>> values) const {
        if (values.size() != size_)
            throw std::invalid_argument("Discovery FFT input does not match its plan length");
        for (std::size_t i = 0; i < size_; ++i) {
            const auto j = permutation_[i];
            if (i < j) std::swap(values[i], values[j]);
        }
        for (std::size_t length = 2; length <= size_; length *= 2) {
            const auto half = length / 2, stride = size_ / length;
            for (std::size_t base = 0; base < size_; base += length) {
                for (std::size_t j = 0; j < half; ++j) {
                    const auto a = values[base + j], b = values[base + j + half];
                    const auto w = twiddles_[j * stride];
                    const std::complex<float> v{
                        b.real() * w.real() - b.imag() * w.imag(),
                        b.real() * w.imag() + b.imag() * w.real()};
                    values[base + j] = a + v;
                    values[base + j + half] = a - v;
                }
            }
        }
    }

private:
    std::size_t size_;
    std::vector<std::uint32_t> permutation_;
    std::vector<std::complex<float>> twiddles_;
};
}
