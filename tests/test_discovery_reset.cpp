// SPDX-License-Identifier: GPL-3.0-or-later
#include "discovery_iq_wipe.hpp"
#include "discovery_repeat.hpp"
#include "discovery_chirp_screen.hpp"
#include "lora_discovery.hpp"
#include "lora_discovery_fixtures.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
using C = std::complex<float>;
namespace f = lora_discovery_fixtures;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void exact_wipe_ranges() {
    constexpr std::size_t capacity = 64;
    constexpr C sentinel{.125f, -.25f};
    for (const auto first : {std::uint64_t{0}, std::uint64_t{7}, std::uint64_t{61},
                              std::numeric_limits<std::uint64_t>::max()})
        for (const auto written : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{7},
                                   std::uint64_t{64}, std::uint64_t{199}}) {
            std::array<C, capacity> ring; ring.fill(sentinel);
            const auto count = std::min<std::uint64_t>(written, capacity);
            require(ovmesh::discovery_detail::wipe_ring(ring, first, written) == count,
                "Wipe work is bounded by written samples, not allocated capacity");
            std::array<bool, capacity> touched{};
            for (std::size_t n = 0; n < count; ++n) touched[(first % capacity + n) % capacity] = true;
            for (std::size_t i = 0; i < capacity; ++i)
                require(ring[i] == (touched[i] ? C{} : sentinel),
                    "Wipe clears every written slot and leaves untouched slots unchanged");
            const auto before = ring;
            for (unsigned i = 0; i < 100; ++i)
                require(ovmesh::discovery_detail::wipe_ring(ring, first, 0) == 0,
                    "Repeated empty reset performs no ring writes");
            require(ring == before, "Empty reset leaves storage untouched");
        }
    require(ovmesh::discovery_detail::wipe_ring({}, 123, 999) == 0,
        "Unallocated history resets without indexing or division");
}

C sample(std::size_t i) {
    return {static_cast<float>((i * 17 % 101) * .002 - .1),
            static_cast<float>((i * 31 % 97) * .002 - .1)};
}

void repetition_reset_equivalence() {
    using Repeat = ovmesh::DiscoveryRepeat;
    auto reused = std::make_unique<Repeat>();
    for (const auto count : {std::size_t{0}, std::size_t{17}, Repeat::capacity - 3, Repeat::capacity * 2 + 19}) {
        for (std::size_t i = 0; i < count; ++i) reused->push({.125f, -.25f});
        reused->reset(); reused->reset();
        auto fresh = std::make_unique<Repeat>();
        for (std::size_t i = 0; i < 4096; ++i) {
            reused->push(sample(i)); fresh->push(sample(i));
            if ((i & 127) == 127)
                for (std::size_t lag = Repeat::min_lag; lag <= Repeat::max_lag; lag *= 2)
                    require(reused->coherence(lag) == fresh->coherence(lag),
                        "Partial/wrapped/full history resets match fresh rolling repetition");
        }
        bool rejected = false;
        try { reused->push({std::numeric_limits<float>::infinity(), 0}); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && reused->coherence(512) == 0, "Invalid repetition sample clears evidence");
    }
}

void lazy_repetition_equivalence() {
    using Repeat = ovmesh::DiscoveryRepeat;
    constexpr std::size_t capacity = Repeat::capacity;
    constexpr std::uint64_t origin = capacity - 31;
    std::vector<C> ring(capacity);
    auto rolling = std::make_unique<Repeat>();
    for (unsigned signal = 0; signal < 4; ++signal) {
        std::fill(ring.begin(), ring.end(), C{});
        rolling->reset();
        std::uint32_t random = 0x81d31265u;
        for (std::size_t i = 0; i < capacity + 19; ++i) {
            C value{};
            if (signal == 0) {
                random = random * 1664525u + 1013904223u;
                const auto real = static_cast<float>(random >> 8) / 16777216.f - .5f;
                random = random * 1664525u + 1013904223u;
                value = {real, static_cast<float>(random >> 8) / 16777216.f - .5f};
            } else if (signal == 1) {
                const auto phase = static_cast<double>(i % 4096);
                const auto angle = std::numbers::pi * phase * phase / 4096.;
                value = {.2f * static_cast<float>(std::cos(angle)), .2f * static_cast<float>(std::sin(angle))};
            } else if (signal == 2) value = {.125f, -.25f};
            rolling->push(value);
            ring[(origin + i) % capacity] = value;
            const auto count = i + 1;
            if (count != 1 && count != 1023 && count != 1024 && count != 8192 &&
                count != capacity - 1 && count != capacity && count != capacity + 19) continue;
            for (std::size_t lag = Repeat::min_lag; lag <= Repeat::max_lag; lag *= 2) {
                const auto expected = rolling->coherence(lag);
                const auto actual = ovmesh::discovery_detail::repetition_coherence(ring, origin, origin + count, lag);
                // Both use float sample products and double sums. The direct
                // sum avoids the rolling subtraction's roundoff history.
                require(std::abs(actual - expected) < 1e-7,
                    "Lazy repetition matches rolling noise/chirp/CW/zero confidence at every lag across wrap");
            }
        }
        const auto next = origin + capacity + 19;
        for (std::size_t i = 0; i < 600; ++i) ring[(next + i) % capacity] = sample(i);
        require(ovmesh::discovery_detail::repetition_coherence(ring, next, next + 600, 512) == 0,
            "A new segment cannot borrow stale pre-gap samples for repetition confidence");
    }
    require(ovmesh::discovery_detail::repetition_coherence({}, 0, 1024, 512) == 0 &&
            ovmesh::discovery_detail::repetition_coherence(ring, 1001, 1000, 512) == 0,
        "Lazy repetition rejects unavailable storage and reversed segment bounds");
}

void chirp_reset_equivalence() {
    using Screen = ovmesh::DiscoveryChirpScreen;
    auto reused = std::make_unique<Screen>();
    for (const auto count : {std::size_t{0}, std::size_t{67}, Screen::capacity + 19}) {
        constexpr std::uint64_t origin = Screen::capacity - 31;
        for (std::size_t i = 0; i < count; ++i) reused->push({.125f, -.25f}, origin + i);
        reused->reset(); reused->reset();
        auto fresh = std::make_unique<Screen>();
        constexpr std::uint64_t next = 5 * Screen::capacity - 63;
        for (std::size_t i = 0; i < 4096; ++i) {
            reused->push(sample(i), next + i); fresh->push(sample(i), next + i);
            if (((next + i + 1) & 127) == 0)
                for (const auto bw : {125000u, 250000u, 500000u}) {
                    const auto actual = reused->statistic(bw, 7, 0);
                    const auto expected = fresh->statistic(bw, 7, 0);
                    require(actual.available == expected.available && actual.correlation == expected.correlation &&
                            actual.dc == expected.dc && actual.energy == expected.energy,
                        "Nonzero origins and wraparound resets match a fresh chirp screen");
                }
        }
        bool rejected = false;
        try { reused->push({}, next); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected && !reused->statistic(500000, 7, 0).available,
            "Backward discontinuity clears chirp screen history");
    }
}

void preamble_boundaries() {
    f::PreambleSpec spec; spec.bandwidth_hz = 500000; spec.spreading_factor = 7;
    spec.center_offset_hz = 20317; spec.preamble_symbols = 10;
    const auto fixture = f::make_preamble(spec);
    const auto cut = static_cast<std::size_t>(fixture.truth.sync_end_sample);
    constexpr std::uint64_t origin = 524288 - 128;
    constexpr std::uint64_t fresh_origin = 4 * 524288;
    for (unsigned boundary = 0; boundary < 4; ++boundary) {
        ovmesh::LoRaPreambleDiscovery reused(907500000), fresh(907500000);
        std::vector<ovmesh::LoRaDiscovery> actual, expected;
        auto receive = [&](const auto& value) { actual.push_back(value); };
        reused.feed(std::span(fixture.samples).first(cut), origin, receive);
        require(actual.empty(), "Preamble without delimiter produces no observation");
        if (boundary == 0) {
            reused.reset(); reused.reset();
            reused.feed(std::span(fixture.samples).subspan(cut), origin + cut, receive);
        } else if (boundary == 1) {
            reused.feed(std::span(fixture.samples).subspan(cut), origin + cut + 1, receive);
        } else if (boundary == 2) {
            reused.feed(std::span(fixture.samples).subspan(cut), 17, receive);
        } else {
            const std::array<C, 1> invalid{{{std::numeric_limits<float>::quiet_NaN(), 0}}};
            reused.feed(invalid, origin + cut, receive);
            reused.feed(std::span(fixture.samples).subspan(cut), origin + cut + 1, receive);
        }
        require(actual.empty(), "No preamble chain survives reset, forward/backward gap, or invalid IQ");
        reused.feed(fixture.samples, fresh_origin, receive);
        fresh.feed(fixture.samples, fresh_origin, [&](const auto& value) { expected.push_back(value); });
        require(actual.size() == 1 && expected.size() == 1, "Whole post-gap preamble remains discoverable");
        require(actual[0].bandwidth_hz == expected[0].bandwidth_hz &&
            actual[0].spreading_factor == expected[0].spreading_factor &&
            actual[0].center_hz == expected[0].center_hz &&
            actual[0].delimiter_sample == expected[0].delimiter_sample,
            "Post-gap discovery is identical to a fresh detector at the same sample origin");
    }
}
}

int main() {
    try {
        exact_wipe_ranges(); repetition_reset_equivalence(); lazy_repetition_equivalence();
        chirp_reset_equivalence(); preamble_boundaries();
        std::cout << "Bounded IQ wipes and fresh-equivalent discovery reset boundaries passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
