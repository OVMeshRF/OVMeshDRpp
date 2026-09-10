// SPDX-License-Identifier: GPL-3.0-or-later
// Independent analytic-tone tests. No codec, preset, device, recording or network.
#include "discovery_channelizer.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using Complex = std::complex<float>;
using Bank = ovmesh::DiscoveryChannelizer;
constexpr double center = 907500000;
constexpr double pi = std::numbers::pi;
void require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }

struct Result { std::vector<std::vector<Complex>> channels; uint64_t first = 0; };
Result receive(Bank& bank, std::span<const Complex> signal, size_t block, uint64_t origin = 0) {
    Result result;
    result.channels.resize(bank.subbands().size());
    uint64_t previous = 0;
    auto callback = [&](const Bank::Samples& samples) {
        require(samples.values.size() <= 4096 / samples.input_sample_stride + 1, "Unbounded callback");
        auto& output = result.channels[samples.subband_index];
        if (samples.subband_index == 0) {
            if (output.empty()) result.first = samples.first_input_sample;
            else require(samples.first_input_sample == previous, "Output time discontinuity");
            previous = samples.first_input_sample + samples.values.size() * samples.input_sample_stride;
        }
        output.insert(output.end(), samples.values.begin(), samples.values.end());
    };
    for (size_t n = 0; n < signal.size(); n += block)
        bank.feed(signal.subspan(n, std::min(block, signal.size() - n)), origin + n, callback);
    return result;
}

std::vector<Complex> tone(uint32_t rate, double hz, size_t count) {
    std::vector<Complex> signal(count);
    for (size_t n = 0; n < count; ++n)
        signal[n] = std::polar(.5f, static_cast<float>(2 * pi * hz * n / rate));
    return signal;
}
double amplitude(std::span<const Complex> signal) {
    double power = 0;
    for (auto value : signal) power += std::norm(value);
    return std::sqrt(power / signal.size());
}
double frequency(std::span<const Complex> signal) {
    std::complex<double> sum{};
    for (size_t n = 1; n < signal.size(); ++n)
        sum += static_cast<std::complex<double>>(signal[n] * std::conj(signal[n - 1]));
    return std::arg(sum) * Bank::output_sample_rate / (2 * pi);
}

void tone_checks() {
    for (uint32_t rate : {8000000U, 10000000U, 12000000U, 16000000U, 20000000U}) {
        for (double relative : {-2000000., -1750000., -1250000., -1000000., -250000.,
                                 0., 250000., 1000000., 1250000., 1750000., 2000000.}) {
            Bank bank(rate, center, center - 2500000, center + 2500000);
            auto input = tone(rate, relative, 16000);
            const auto output = receive(bank, input, 503, 70000);
            require(output.first >= 70000, "FIR timestamps precede supplied input");
            require(bank.prototype_tap_count() == static_cast<size_t>(rate / 1000000) * 32 + 1,
                    "Unexpected prototype length");
            for (size_t channel = 0; channel < bank.subbands().size(); ++channel) {
                const auto offset = center + relative - bank.subbands()[channel].center_hz;
                const auto& samples = output.channels[channel];
                require(!samples.empty(), "Missing output channel");
                const double gain = amplitude(samples) / .5;
                if (std::abs(offset) <= 750000) {
                    require(std::abs(gain - 1) < .002, "Declared flat passband attenuates a tone");
                    require(std::abs(frequency(samples) - offset) < 8, "Output has wrong center or phase rotation");
                } else if (std::abs(offset) >= 1000000) {
                    require(gain < .001, "Out-of-band tone aliases into a discovery output");
                }
            }
        }
    }
}

void coverage_checks() {
    Bank bank(16000000, center, center - 6000000, center + 6000000);
    require(bank.subbands().size() == 13, "Unexpected 12 MHz subband plan");
    for (double width : {125000., 250000., 500000.}) {
        for (double frequency_hz = center - 6000000 + width / 2;
             frequency_hz <= center + 6000000 - width / 2; frequency_hz += 12345.25) {
            require(std::any_of(bank.subbands().begin(), bank.subbands().end(), [&](auto channel) {
                return frequency_hz - width / 2 >= channel.passband_lower_hz &&
                       frequency_hz + width / 2 <= channel.passband_upper_hz;
            }), "A full-width chirp falls through a subband boundary");
        }
    }
}

void stream_checks() {
    const auto input = tone(16000000, 1123456, 17000);
    Bank whole(16000000, center, center - 2000000, center + 2000000);
    Bank chunks(16000000, center, center - 2000000, center + 2000000);
    const auto a = receive(whole, input, input.size(), 99999);
    const auto b = receive(chunks, input, 1, 99999);
    require(a.first == b.first && a.channels == b.channels, "Chunking changes filter samples or timing");
    bool gap_rejected = false;
    try { whole.feed(input, 1, {}); } catch (const std::invalid_argument&) { gap_rejected = true; }
    require(gap_rejected, "Input gap was silently bridged");
    whole.reset();
    const auto c = receive(whole, input, 731, 99999);
    require(a.first == c.first && a.channels == c.channels, "Reset retains sample state");
    whole.reset();
    Complex invalid{std::numeric_limits<float>::quiet_NaN(), 0};
    bool nonfinite_rejected = false;
    try { whole.feed(std::span(&invalid, 1), 0, {}); }
    catch (const std::invalid_argument&) { nonfinite_rejected = true; }
    require(nonfinite_rejected, "Non-finite input contaminated the filter");
    const auto d = receive(whole, input, 300, 99999);
    require(a.channels == d.channels, "Non-finite reset was incomplete");

    // An isolated impulse locates the actual filter center independently of
    // callback chunking and verifies the declared group-delay compensation.
    std::vector<Complex> impulse(4096);
    constexpr uint64_t at = 2000, origin = 17000;
    impulse[at] = {1, 0};
    Bank timed(16000000, center, center - 1000000, center + 1000000);
    const auto response = receive(timed, impulse, 53, origin);
    const auto zero = std::find_if(timed.subbands().begin(), timed.subbands().end(),
                                  [](auto subband) { return subband.center_hz == center; });
    const auto& samples = response.channels[static_cast<size_t>(zero - timed.subbands().begin())];
    const auto maximum = std::max_element(samples.begin(), samples.end(),
                                         [](auto x, auto y) { return std::norm(x) < std::norm(y); });
    const auto maximum_time = response.first + static_cast<size_t>(maximum - samples.begin()) * 8;
    require(std::abs(static_cast<double>(maximum_time) - static_cast<double>(origin + at)) <= 4,
            "FIR impulse center disagrees with reported sample coordinate");

    Bank moved(std::move(timed));
    require(moved.subbands().size() == response.channels.size(), "Moving loses channel metadata");
}

void benchmark() {
    Bank bank(16000000, center, center - 6000000, center + 6000000);
    std::vector<Complex> input(131072, Complex{.2f, .1f});
    uint64_t count = 0;
    const auto started = std::chrono::steady_clock::now();
    constexpr size_t blocks = 64;
    for (size_t n = 0; n < blocks; ++n)
        bank.feed(input, n * input.size(), [&](const Bank::Samples& samples) { count += samples.values.size(); });
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto input_seconds = static_cast<double>(blocks * input.size()) / 16000000;
    std::cout << "PFB benchmark: input_s=" << input_seconds << " wall_s=" << seconds
              << " realtime_ratio=" << input_seconds / seconds << " output_samples=" << count << '\n';
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--benchmark") { benchmark(); return 0; }
        tone_checks(); coverage_checks(); stream_checks();
        std::cout << "Discovery channelizer tone, passband, alias, coverage and stream tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
