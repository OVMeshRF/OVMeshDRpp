// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/discovery_fft.hpp"
#include "ovmesh/phy.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>
#include <string_view>

namespace {
using C = std::complex<float>;
using D = std::complex<double>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<C> random_input(std::size_t size) {
    std::mt19937 random(static_cast<std::uint32_t>(0x6a95217u + size));
    std::uniform_real_distribution<float> uniform(-1, 1);
    std::vector<C> result(size);
    for (auto& value : result) value = {uniform(random), uniform(random)};
    return result;
}

std::vector<D> direct_dft(std::span<const C> input) {
    std::vector<D> result(input.size());
    for (std::size_t k = 0; k < input.size(); ++k)
        for (std::size_t j = 0; j < input.size(); ++j) {
            const double phase = -2 * std::numbers::pi * static_cast<double>(k * j) /
                static_cast<double>(input.size());
            result[k] += D(input[j]) * D(std::cos(phase), std::sin(phase));
        }
    return result;
}

void small_oracles() {
    for (std::size_t size : {2u, 4u, 8u, 16u, 32u, 64u}) {
        const ovmesh::DiscoveryFftPlan plan(size);
        const auto input = random_input(size), original = input;
        const auto expected = direct_dft(input);
        auto actual = input;
        plan.execute(actual);
        for (std::size_t i = 0; i < size; ++i)
            require(std::abs(D(actual[i]) - expected[i]) < 2e-6 * static_cast<double>(size),
                    "Cached FFT disagrees with independent double-precision DFT");
        require(input == original, "Comparison source changed");

        std::vector<C> impulse(size); impulse[0] = {1, 0};
        plan.execute(impulse);
        for (auto bin : impulse) require(std::abs(bin - C{1, 0}) < 1e-6f,
                                       "Forward FFT must be unnormalized");

        const std::size_t target = size - 1;
        std::vector<C> tone(size);
        for (std::size_t i = 0; i < size; ++i) {
            const double phase = 2 * std::numbers::pi * static_cast<double>(target * i) /
                static_cast<double>(size);
            tone[i] = {static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
        }
        plan.execute(tone);
        for (std::size_t k = 0; k < size; ++k)
            require(std::abs(tone[k] - C{k == target ? static_cast<float>(size) : 0, 0}) <
                    static_cast<float>(size) * 2e-6f, "Forward FFT sign or bin order is incorrect");
    }
}

void reference_and_roundtrip() {
    double largest_relative_error = 0, largest_roundtrip_error = 0;
    for (std::size_t size = 2; size <= ovmesh::DiscoveryFftPlan::maximum_size; size *= 2) {
        const ovmesh::DiscoveryFftPlan plan(size);
        require(plan.size() == size, "FFT plan length changed");
        const auto input = random_input(size);
        auto actual = input, reference = input;
        plan.execute(actual);
        ovmesh::fft_inplace(reference);
        double error_power = 0, reference_power = 0;
        for (std::size_t i = 0; i < size; ++i) {
            error_power += std::norm(D(actual[i]) - D(reference[i]));
            reference_power += std::norm(D(reference[i]));
        }
        const double relative_error = std::sqrt(error_power / reference_power);
        largest_relative_error = std::max(largest_relative_error, relative_error);
        require(relative_error < 2e-6, "Cached FFT exceeds relative reference error tolerance");
        for (auto& value : actual) value = std::conj(value);
        plan.execute(actual);
        for (std::size_t i = 0; i < size; ++i) {
            const D recovered = std::conj(D(actual[i])) / static_cast<double>(size);
            largest_roundtrip_error = std::max(largest_roundtrip_error, std::abs(recovered - D(input[i])));
            require(std::abs(recovered - D(input[i])) < 6e-6, "Cached forward/conjugate inverse roundtrip failed");
        }
    }
    std::cout << "Maximum relative reference error=" << largest_relative_error
              << " maximum roundtrip error=" << largest_roundtrip_error << '\n';
}

void reject_invalid() {
    for (std::size_t size : {0u, 1u, 3u, 1023u, 131073u, 524288u}) {
        bool rejected = false;
        try { const ovmesh::DiscoveryFftPlan plan(size); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Invalid FFT plan size was accepted");
    }
    const ovmesh::DiscoveryFftPlan plan(8);
    auto values = random_input(4); const auto original = values;
    bool rejected = false;
    try { plan.execute(values); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && values == original, "Wrong-length input must be rejected without modification");
}

template<class Transform>
double timed_transforms(const std::vector<C>& input, std::size_t iterations, Transform transform,
                        double& checksum) {
    auto work = input;
    transform(work); // Warm the plan/code before timing.
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        std::copy(input.begin(), input.end(), work.begin());
        transform(work);
        checksum += work[i % work.size()].real();
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void benchmark() {
    std::cout << "Synthetic cached-plan benchmark; plan construction excluded, input copy included.\n"
              << "size,iterations,cached_ms,existing_ms,speedup\n";
    double checksum = 0;
    for (std::size_t size = 1024; size <= ovmesh::DiscoveryFftPlan::maximum_size; size *= 2) {
        const ovmesh::DiscoveryFftPlan plan(size); const auto input = random_input(size);
        const auto iterations = std::max<std::size_t>(4, 524288 / size);
        std::array<double, 3> cached{}, existing{};
        for (std::size_t trial = 0; trial < cached.size(); ++trial) {
            cached[trial] = timed_transforms(input, iterations, [&](auto& values) { plan.execute(values); }, checksum);
            existing[trial] = timed_transforms(input, iterations, [](auto& values) { ovmesh::fft_inplace(values); }, checksum);
        }
        std::sort(cached.begin(), cached.end()); std::sort(existing.begin(), existing.end());
        std::cout << size << ',' << iterations << ',' << cached[1] << ',' << existing[1]
                  << ',' << existing[1] / cached[1] << '\n';
    }
    std::cout << "Benchmark checksum=" << checksum << '\n';
}
}

int main(int argc, char** argv) {
    try {
        if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--benchmark"))
            throw std::invalid_argument("Usage: test_discovery_fft [--benchmark]");
        small_oracles(); reference_and_roundtrip(); reject_invalid();
        if (argc == 2) benchmark();
        std::cout << "Discovery FFT plan checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
