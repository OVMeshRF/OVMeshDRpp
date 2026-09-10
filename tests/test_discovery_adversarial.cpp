// SPDX-License-Identifier: GPL-3.0-or-later
#include "lora_discovery.hpp"
#include "lora_discovery_fixtures.hpp"

#include <array>
#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace {
namespace f = lora_discovery_fixtures;
using ovmesh::LoRaDiscovery;
constexpr double center_hz = 907'500'000;
constexpr double sample_rate = 2'000'000;
constexpr std::uint64_t origin = 30'000'017;

struct Run {
    std::vector<LoRaDiscovery> found;
    ovmesh::LoRaDiscoveryStats stats;
};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void feed(ovmesh::LoRaPreambleDiscovery& detector, std::span<const f::Complex> input,
          std::uint64_t first, std::vector<LoRaDiscovery>& results, std::size_t chunk = 4093) {
    for (std::size_t offset = 0; offset < input.size(); offset += chunk)
        detector.feed(input.subspan(offset, std::min(chunk, input.size() - offset)), first + offset,
            [&](const auto& result) { results.push_back(result); });
}

Run detect(std::span<const f::Complex> samples, std::size_t chunk = 4093) {
    ovmesh::LoRaPreambleDiscovery detector(center_hz);
    Run run;
    feed(detector, samples, origin, run.found, chunk);
    run.stats = detector.stats();
    return run;
}

std::string describe(const std::vector<LoRaDiscovery>& found) {
    std::ostringstream text;
    text << std::setprecision(9) << found.size() << " detections";
    for (const auto& item : found)
        text << " [" << item.bandwidth_hz << "/SF" << item.spreading_factor
             << " offset=" << item.center_hz - center_hz
             << " delimiter=" << item.delimiter_sample - static_cast<double>(origin)
             << " up=" << item.up_match << " down=" << item.down_match << ']';
    return text.str();
}

bool matches(const LoRaDiscovery& found, const f::FrameTruth& truth) {
    const double tolerance_hz = sample_rate / truth.symbol_samples;
    return found.bandwidth_hz == truth.bandwidth_hz && found.spreading_factor == truth.spreading_factor &&
        std::abs(found.center_hz - center_hz - truth.received_center_offset_hz) < tolerance_hz;
}

void require_one(const Run& run, const f::FrameTruth& truth) {
    require(run.found.size() == 1 && matches(run.found.front(), truth),
            "Expected one correct BW/SF and center within one dechirped bin; " + describe(run.found));
    require(run.found.front().first_observed_upchirp_sample >= static_cast<double>(origin),
            "Preamble estimate precedes the observed stream origin");
    require(run.found.front().delimiter_sample > run.found.front().first_observed_upchirp_sample,
            "Delimiter is not later than observed preamble");
}

struct Suite {
    unsigned passed = 0, failed = 0;
    void test(const std::string& name, const std::function<void()>& work) {
        try { work(); ++passed; std::cout << "PASS " << name << std::endl; }
        catch (const std::exception& error) {
            ++failed;
            std::cout << "FAIL " << name << ": " << error.what() << std::endl;
        }
    }
};

void phase_and_aliases(Suite& suite) {
    // Each group includes hypotheses with exactly equal B^2/2^SF. A short
    // linear ramp is insufficient to distinguish them, so any extra alias is
    // a correctness failure, even if the correct hypothesis also appears.
    const std::array<std::pair<unsigned, unsigned>, 6> profiles{{
        {125'000, 7}, {250'000, 9}, {500'000, 11},
        {125'000, 9}, {250'000, 11}, {500'000, 12}}};
    unsigned index = 0;
    for (const auto& [bandwidth, sf] : profiles) {
        // Include starts both near and between the detector's quarter-symbol
        // search phases, with noninteger chip timing and changing carrier phase.
        for (double phase : {0., .1234567, .24991, .371337, .50013, .625, .87491, .99987}) {
            f::PreambleSpec spec;
            spec.bandwidth_hz = bandwidth;
            spec.spreading_factor = sf;
            const double n = sample_rate * (1u << sf) / bandwidth;
            const double start = 127 + phase * n;
            spec.leading_samples = static_cast<std::size_t>(start);
            spec.fractional_start_samples = start - static_cast<double>(spec.leading_samples);
            spec.center_offset_hz = index % 2 ? -411'393. : 401'813.;
            spec.cfo_hz = index % 3 ? 913.375 : -1'319.625;
            spec.initial_phase_radians = .191 * index;
            spec.sync_word = std::array<std::uint8_t, 3>{0x12, 0x34, 0x2b}[index % 3];
            spec.noise_rms = .02;
            spec.seed = 7189 + index * 109;
            std::ostringstream label;
            label << "alias/phase " << bandwidth << "/SF" << sf << " phase=" << phase;
            suite.test(label.str(), [&] {
                const auto fixture = f::make_preamble(spec);
                const auto run = detect(fixture.samples, index % 2 ? 997 : 4093);
                require_one(run, fixture.truth);
                require(run.stats.samples == fixture.samples.size(), "Input accounting mismatch");
            });
            ++index;
        }
    }
    for (unsigned count : {8u, 16u, 32u, 64u}) {
        for (unsigned bandwidth : {250'000u, 500'000u}) {
            suite.test("0x2b preamble=" + std::to_string(count) + " BW=" + std::to_string(bandwidth), [&] {
                f::PreambleSpec spec;
                spec.bandwidth_hz = bandwidth;
                spec.spreading_factor = 11;
                spec.sync_word = 0x2b;
                spec.preamble_symbols = count;
                spec.center_offset_hz = 412'793;
                spec.cfo_hz = 619.375;
                spec.leading_samples = 8'293;
                spec.fractional_start_samples = .371;
                spec.noise_rms = .02;
                const auto fixture = f::make_preamble(spec);
                require_one(detect(fixture.samples), fixture.truth);
            });
        }
    }
    for (const auto& [bandwidth, sf] : std::array<std::pair<unsigned, unsigned>, 3>{{
             {125'000, 12}, {250'000, 11}, {500'000, 7}}}) {
        suite.test("noiseless fractional-bin exact-one " + std::to_string(bandwidth) + "/SF" + std::to_string(sf), [&] {
            f::PreambleSpec spec;
            spec.bandwidth_hz = bandwidth;
            spec.spreading_factor = sf;
            spec.center_offset_hz = 391'337.179;
            spec.fractional_start_samples = .413;
            const auto fixture = f::make_preamble(spec);
            require_one(detect(fixture.samples), fixture.truth);
        });
    }
}

void no_frame_negatives(Suite& suite) {
    constexpr std::size_t count = 400'000;
    for (std::uint64_t seed : {31u, 1579u, 20981u}) {
        suite.test("noise seed=" + std::to_string(seed), [&] {
            std::vector<f::Complex> samples(count);
            f::add_noise(samples, .15, seed);
            const auto found = detect(samples).found;
            require(found.empty(), "Noise became a LoRa discovery: " + describe(found));
        });
    }
    suite.test("periodically gated two-tone CW", [&] {
        std::vector<f::Complex> samples(count);
        f::add_cw(samples, sample_rate, -272'179, .3);
        f::add_cw(samples, sample_rate, 461'123, .15);
        for (std::size_t i = 0; i < samples.size(); ++i)
            if ((i / 16384) % 2) samples[i] = {};
        f::add_noise(samples, .02, 719);
        const auto found = detect(samples).found;
        require(found.empty(), "Gated CW became a LoRa discovery: " + describe(found));
    });
    for (double bit_rate : {7'319., 15'625., 125'000.}) {
        suite.test("FSK symbol rate=" + std::to_string(bit_rate), [&] {
            std::vector<f::Complex> samples(count);
            f::FskSpec spec;
            spec.symbol_rate_hz = bit_rate;
            spec.center_offset_hz = -212'731;
            spec.deviation_hz = 61'127;
            f::add_fsk(samples, spec);
            f::add_noise(samples, .02, 271);
            const auto found = detect(samples).found;
            require(found.empty(), "FSK became a LoRa discovery: " + describe(found));
        });
    }
    suite.test("noiseless fractional CW plus periodic FSK", [&] {
        std::vector<f::Complex> samples(count);
        f::FskSpec spec;
        spec.center_offset_hz = -212'731.471;
        spec.deviation_hz = 62'500;
        spec.symbol_rate_hz = 31'250;
        spec.amplitude = .5;
        f::add_fsk(samples, spec);
        f::add_cw(samples, sample_rate, 419'391.371, .8);
        const auto found = detect(samples).found;
        require(found.empty(), "CW/FSK mixture became a LoRa discovery: " + describe(found));
    });
    for (unsigned kind = 0; kind < 5; ++kind) {
        suite.test(std::array<std::string, 5>{"preamble without SFD", "one downchirp missing",
            "second downchirp frequency changed", "SFD entirely replaced by CW",
            "all downchirps shifted relative to preamble"}[kind], [&] {
            f::PreambleSpec spec;
            spec.center_offset_hz = 312'491;
            spec.spreading_factor = 10;
            spec.fractional_start_samples = .419;
            const auto fixture = f::make_preamble(spec);
            auto samples = fixture.samples;
            const auto first_down = static_cast<std::size_t>(std::ceil(fixture.truth.sync_end_sample));
            const auto second_down = static_cast<std::size_t>(std::ceil(
                fixture.truth.sync_end_sample + fixture.truth.symbol_samples));
            const auto third_quarter = static_cast<std::size_t>(std::ceil(
                fixture.truth.sync_end_sample + 2 * fixture.truth.symbol_samples));
            if (kind == 0 || kind == 3) {
                std::fill(samples.begin() + static_cast<std::ptrdiff_t>(first_down), samples.end(), f::Complex{});
                if (kind == 3)
                    f::add_cw(std::span(samples).subspan(first_down), sample_rate, spec.center_offset_hz, spec.amplitude);
            } else if (kind == 1) {
                std::fill(samples.begin() + static_cast<std::ptrdiff_t>(second_down),
                          samples.begin() + static_cast<std::ptrdiff_t>(third_quarter), f::Complex{});
            } else {
                const auto begin = kind == 4 ? first_down : second_down;
                const auto end = kind == 4 ? fixture.truth.end_sample : third_quarter;
                for (std::size_t i = begin; i < end; ++i) {
                    const double phase = 2 * std::numbers::pi * (spec.bandwidth_hz / 8.) *
                        static_cast<double>(i - begin) / sample_rate;
                    samples[i] *= std::polar(1.f, static_cast<float>(phase));
                }
            }
            f::add_noise(samples, .01, 19019);
            const auto found = detect(samples).found;
            require(found.empty(), "Incomplete/inconsistent SFD confirmed: " + describe(found));
        });
    }
}

void gaps_and_partitioning(Suite& suite) {
    f::PreambleSpec spec;
    spec.spreading_factor = 11;
    spec.bandwidth_hz = 250'000;
    spec.center_offset_hz = -387'431;
    spec.fractional_start_samples = .713;
    spec.noise_rms = .01;
    const auto fixture = f::make_preamble(spec);
    suite.test("partition invariance and absolute sample origin", [&] {
        const auto a = detect(fixture.samples, 1'003);
        const auto b = detect(fixture.samples, 32'768);
        require_one(a, fixture.truth);
        require_one(b, fixture.truth);
        require(a.found.front().center_hz == b.found.front().center_hz &&
                a.found.front().delimiter_sample == b.found.front().delimiter_sample &&
                a.stats.windows == b.stats.windows && a.stats.fft_searches == b.stats.fft_searches,
                "Chunk boundaries changed detector evidence");
    });
    for (bool explicit_reset : {false, true}) {
        suite.test(explicit_reset ? "explicit reset discards prior preamble" : "timestamp gap discards prior preamble", [&] {
            ovmesh::LoRaPreambleDiscovery detector(center_hz);
            std::vector<LoRaDiscovery> found;
            const auto split = static_cast<std::size_t>(std::ceil(fixture.truth.sync_end_sample));
            feed(detector, std::span(fixture.samples).first(split), origin, found);
            if (explicit_reset) detector.reset();
            feed(detector, std::span(fixture.samples).subspan(split),
                 origin + split + (explicit_reset ? 0 : 877), found);
            require(found.empty(), "Detector joined preamble/SFD across reset: " + describe(found));
            require(detector.stats().resets == 1 && detector.stats().samples == fixture.samples.size(),
                    "Gap/reset accounting mismatch");
        });
    }
    for (bool nan_value : {false, true}) {
        suite.test(nan_value ? "NaN after armed preamble discards frame and recovers" :
                              "infinity after armed preamble discards frame and recovers", [&] {
            ovmesh::LoRaPreambleDiscovery detector(center_hz);
            Run run;
            auto damaged = fixture.samples;
            const auto index = static_cast<std::size_t>(std::ceil(fixture.truth.sync_end_sample)) - 1;
            if (nan_value) damaged[index].real(std::numeric_limits<float>::quiet_NaN());
            else damaged[index].imag(std::numeric_limits<float>::infinity());
            feed(detector, damaged, origin, run.found);
            require(run.found.empty(), "Nonfinite input allowed a stale preamble confirmation: " + describe(run.found));
            require(detector.stats().resets >= 1, "Nonfinite input did not record a discontinuity");
            feed(detector, fixture.samples, origin + damaged.size(), run.found);
            run.stats = detector.stats();
            require_one(run, fixture.truth);
            require(run.found.front().first_observed_upchirp_sample >=
                    static_cast<double>(origin + damaged.size()),
                    "Recovered detection reused pre-discontinuity evidence");
            require(run.stats.samples == damaged.size() + fixture.samples.size(),
                    "Nonfinite recovery lost input accounting");
        });
    }
}

void sensitivity_diagnostics(Suite& suite) {
    // These are explicit diagnostic cases, not assumed coverage promises. A
    // miss is reported, while any wrong hypothesis or unsupported center fails.
    for (unsigned kind = 0; kind < 7; ++kind) {
        const std::string name = std::array<std::string, 7>{"equal strength distinct profiles",
            "equal strength same profile", "unequal strength same profile",
            "strong off-channel CW", "weak LoRa in noise", "whole-band repeat cancellation",
            "single-lag chirp-screen cancellation"}[kind];
        suite.test("diagnostic correctness: " + name, [&] {
            f::PreambleSpec a;
            a.spreading_factor = 11;
            a.bandwidth_hz = 250'000;
            a.center_offset_hz = -321'131;
            a.noise_rms = 0;
            a.fractional_start_samples = .31;
            const auto first = f::make_preamble(a);
            auto samples = first.samples;
            std::vector<f::FrameTruth> expected{first.truth};
            if (kind <= 2 || kind >= 5) {
                f::PreambleSpec b = a;
                b.center_offset_hz = 332'917;
                b.initial_phase_radians = 1.23;
                b.bandwidth_hz = kind == 0 ? 500'000 : 250'000;
                b.amplitude = kind == 2 ? .06 : .2;
                if (kind == 5) b.center_offset_hz = a.center_offset_hz + 4'000.5 / first.truth.symbol_seconds;
                if (kind == 6) b.center_offset_hz = a.center_offset_hz + 500.5 * 8 / first.truth.symbol_seconds;
                const auto second = f::make_preamble(b);
                samples.resize(std::max(samples.size(), second.samples.size()));
                for (std::size_t i = 0; i < second.samples.size(); ++i) samples[i] += second.samples[i];
                expected.push_back(second.truth);
            } else if (kind == 3) {
                f::add_cw(samples, sample_rate, 419'391, .8);
            }
            f::add_noise(samples, kind == 4 ? .4 : .02, 9127);
            const auto run = detect(samples);
            for (const auto& found : run.found)
                require(std::any_of(expected.begin(), expected.end(), [&](const auto& truth) { return matches(found, truth); }),
                        "Fabricated BW/SF/center in mixture: " + describe(run.found));
            std::size_t recovered = 0;
            for (const auto& truth : expected) {
                const auto count = std::count_if(run.found.begin(), run.found.end(), [&](const auto& found) { return matches(found, truth); });
                require(count <= 1, "Duplicate discovery for one signal in mixture: " + describe(run.found));
                recovered += count != 0;
            }
            std::cout << "DIAGNOSTIC " << name << ": recovered " << recovered << '/' << expected.size()
                      << "; " << describe(run.found) << std::endl;
            if (kind >= 5)
                require(recovered == expected.size(), "Whole-band cancellation hid independent components");
            // These formerly diagnostic misses now have component-aware fixes;
            // preserve their demonstrated recovery as an explicit regression.
            if (kind <= 3)
                require(recovered == expected.size(), "A validated simultaneous/near-far component was missed");
        });
    }
    suite.test("reported bounded candidate capacity", [&] {
        std::vector<f::Complex> samples;
        std::vector<f::FrameTruth> expected;
        for (unsigned i = 0; i < 9; ++i) {
            f::PreambleSpec spec;
            spec.bandwidth_hz = 125'000;
            spec.spreading_factor = 7;
            spec.center_offset_hz = -520'000. + 130'000. * i;
            spec.fractional_start_samples = .619;
            spec.amplitude = .04;
            const auto fixture = f::make_preamble(spec);
            samples.resize(std::max(samples.size(), fixture.samples.size()));
            for (std::size_t k = 0; k < fixture.samples.size(); ++k) samples[k] += fixture.samples[k];
            expected.push_back(fixture.truth);
        }
        f::add_noise(samples, .001, 1931);
        const auto run = detect(samples);
        require(run.stats.candidate_limit_hits > 0 || run.stats.track_limit_hits > 0,
                "More qualified components than the configured bound were not reported");
        for (const auto& found : run.found)
            require(std::any_of(expected.begin(), expected.end(), [&](const auto& truth) { return matches(found, truth); }),
                    "Capacity pressure fabricated a component: " + describe(run.found));
        std::cout << "CAPACITY candidates=" << run.stats.candidate_limit_hits
                  << " tracks=" << run.stats.track_limit_hits << "; " << describe(run.found) << std::endl;
    });
    for (unsigned bandwidth : {250'000u, 500'000u}) for (double ppm : {-20., 20.}) {
        suite.test("clock diagnostic " + std::to_string(bandwidth) + " ppm=" + std::to_string(ppm), [&] {
            f::PreambleSpec spec;
            spec.sample_rate_hz = sample_rate * (1 + ppm * 1e-6);
            spec.bandwidth_hz = bandwidth;
            spec.spreading_factor = 11;
            spec.preamble_symbols = 16;
            spec.sync_word = 0x2b;
            spec.center_offset_hz = -391'731;
            spec.fractional_start_samples = .371;
            spec.noise_rms = .02;
            const auto fixture = f::make_preamble(spec);
            auto apparent = fixture.truth;
            // Generated sample times use the perturbed clock; the detector's
            // nominal 2 MS/s axis therefore rescales received frequency. BW
            // remains the nominal hypothesis, not a clock-calibration result.
            apparent.received_center_offset_hz *= sample_rate / spec.sample_rate_hz;
            const auto run = detect(fixture.samples);
            for (const auto& found : run.found)
                require(matches(found, apparent), "Clock offset caused wrong BW/SF/center: " + describe(run.found));
            require(run.found.size() <= 1, "Clock offset caused duplicate discoveries");
            std::cout << "CLOCK ppm=" << ppm << " BW=" << bandwidth << " expected_offset="
                      << apparent.received_center_offset_hz << "; " << describe(run.found) << std::endl;
        });
    }
}

void benchmark() {
    constexpr std::size_t count = 1'000'000;
    for (unsigned kind = 0; kind < 4; ++kind) {
        std::vector<f::Complex> samples(count);
        if (kind >= 2) {
            f::PreambleSpec spec;
            spec.center_offset_hz = -321'131;
            spec.spreading_factor = 11;
            spec.preamble_symbols = 16;
            spec.sync_word = 0x2b;
            const auto fixture = f::make_preamble(spec);
            std::copy(fixture.samples.begin(), fixture.samples.end(), samples.begin());
            if (kind == 3) {
                spec.center_offset_hz = 332'917;
                const auto second = f::make_preamble(spec);
                for (std::size_t i = 0; i < second.samples.size(); ++i) samples[i] += second.samples[i];
            }
        }
        if (kind == 1 || kind == 2) f::add_cw(samples, sample_rate, 419'391, .8);
        f::add_noise(samples, .02, 7913);
        const auto start = std::chrono::steady_clock::now();
        const auto run = detect(samples);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "BENCH " << std::array<const char*, 4>{"noise", "CW", "LoRa plus CW", "two LoRa"}[kind]
                  << " input_s=" << static_cast<double>(samples.size()) / sample_rate
                  << " wall_s=" << elapsed << " fft=" << run.stats.fft_searches
                  << " found=" << run.found.size() << " candidate_limit=" << run.stats.candidate_limit_hits
                  << " track_limit=" << run.stats.track_limit_hits << std::endl;
    }
}
} // namespace

int main(int argc, char** argv) {
    const std::string selection = argc == 1 ? "all" : argv[1];
    if (argc > 2 || (selection != "all" && selection != "phase" && selection != "negatives" && selection != "benchmark" &&
                    selection != "gaps" && selection != "mixed")) {
        std::cerr << "Usage: test-discovery-adversarial [all|phase|negatives|gaps|mixed|benchmark]\n";
        return 2;
    }
    if (selection == "benchmark") { benchmark(); return 0; }
    Suite suite;
    if (selection == "all" || selection == "phase") phase_and_aliases(suite);
    if (selection == "all" || selection == "negatives") no_frame_negatives(suite);
    if (selection == "all" || selection == "gaps") gaps_and_partitioning(suite);
    if (selection == "all" || selection == "mixed") sensitivity_diagnostics(suite);
    std::cout << "Adversarial correctness: " << suite.passed << " passed, " << suite.failed << " failed\n";
    return suite.failed ? 1 : 0;
}
