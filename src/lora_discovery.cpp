// SPDX-License-Identifier: GPL-3.0-or-later
#include "lora_discovery.hpp"
#include "discovery_fft.hpp"
#include "discovery_repeat.hpp"
#include "discovery_chirp_screen.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace ovmesh {
namespace {
using C = std::complex<float>;
constexpr double rate = 2'000'000;
constexpr size_t fft_capacity = 131072;
// A candidate may need up to one more phase checkpoint before both complete
// delimiter symbols are available. Retain four longest symbols for that check.
constexpr size_t capacity = 262144;
constexpr size_t max_components = 8;
struct Hypothesis {
    uint32_t bw;
    unsigned sf;
    size_t n;
    std::vector<C> up;
};
const std::vector<Hypothesis>& bank() {
    static const auto result = [] {
        std::vector<Hypothesis> v;
        for (uint32_t bw : {125000u, 250000u, 500000u}) {
            for (unsigned sf = 7; sf <= 12; ++sf) {
                const auto n = static_cast<size_t>(rate / bw) * (size_t{1} << sf);
                Hypothesis h{bw, sf, n, std::vector<C>(n)};
                for (size_t i = 0; i < n; ++i) {
                    const double t = static_cast<double>(i) / rate;
                    const double cycles = -.5 * bw * t + .5 * bw * rate / static_cast<double>(n) * t * t;
                    const double a = 2 * std::numbers::pi * std::remainder(cycles, 1.);
                    h.up[i] = C(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
                }
                v.push_back(std::move(h));
            }
        }
        return v;
    }();
    return result;
}
const DiscoveryFftPlan& fft_plan(size_t length) {
    static const auto plans = [] {
        std::vector<DiscoveryFftPlan> result;
        for (size_t n = 1024; n <= fft_capacity; n *= 2) result.emplace_back(n);
        return result;
    }();
    size_t index = 0;
    for (size_t n = 1024; n < length; n *= 2) ++index;
    return plans.at(index);
}
struct Tone { double hz = 0, match = 0, power = 0, background = 0; };
struct WrapFit { double center = 0, delta = 0, match = 0, component_power = 0; };
struct WrapFits { std::array<WrapFit, max_components> values{}; size_t size = 0; };
struct Chain {
    unsigned streak = 0;
    double last_up_hz = 0, sum_up_hz = 0, up_coherence = 0;
    double up_match = 0;
    uint64_t streak_start = 0, last_up_end = 0;
    bool armed = false;
    double armed_hz = 0, armed_match = 0, armed_coherence = 0;
    uint64_t armed_first = 0, armed_end = 0;
    WrapFits reset_fits;
    unsigned down_count = 0;
    double down_hz = 0, down_match = 0;
    double down_background = 0;
    uint64_t down_end = 0;
    bool pending = false;
    LoRaDiscovery pending_found{};
    double pending_component_power = 0, pending_first_background = 0, pending_second_background = 0;
};
}

struct LoRaPreambleDiscovery::Impl {
    double center;
    std::array<C, capacity> ring{};
    DiscoveryRepeat repetition;
    DiscoveryChirpScreen screen;
    uint64_t end = 0, begin = 0;
    bool initialized = false;
    using Phase = std::array<Chain, max_components>;
    std::vector<std::array<Phase, 4>> chains{bank().size()};
    std::vector<C> work;
    std::vector<double> spectral_prefix;
    std::vector<std::complex<double>> prefix_high, prefix_low;
    std::vector<LoRaDiscovery> recent;
    LoRaDiscoveryStats counters;
    explicit Impl(double frequency) : center(frequency) {
        if (!std::isfinite(center)) throw std::invalid_argument("Discovery center is not finite");
        work.reserve(fft_capacity);
    }
    C sample(uint64_t index) const { return ring[index % capacity]; }
    double coherence(size_t n) const {
        return repetition.coherence(n);
    }
    bool chirp_screen(const Hypothesis& h) const {
        return screen.passes(h.bw, h.sf);
    }
    double spectral_sum(size_t first, size_t count) const {
        const size_t n = work.size();
        first %= n;
        const size_t initial = std::min(count, n - first);
        return spectral_prefix[first + initial] - spectral_prefix[first] +
            (count > initial ? spectral_prefix[count - initial] : 0.);
    }
    double local_background(size_t index) const {
        // The main lobe of a twice-zero-padded rectangular DFT occupies roughly
        // +/-2 bins. Nearby side-lobe candidates include the stronger main lobe
        // in their reference region, avoiding duplicate component tracks.
        constexpr size_t radius = 32, guard = 2;
        const size_t n = work.size();
        const double reference = spectral_sum((index + n - radius) % n, radius - guard) +
                                 spectral_sum((index + guard + 1) % n, radius - guard);
        const double mean = reference / (2 * static_cast<double>(radius - guard));
        return std::max(mean, spectral_prefix.back() / static_cast<double>(n) * 1e-12);
    }
    double background_at(double hz) const {
        const auto n = static_cast<long long>(work.size());
        const auto index = std::llround(hz * static_cast<double>(n) / rate);
        return local_background(static_cast<size_t>((index % n + n) % n));
    }
    std::vector<Tone> tones(const Hypothesis& h, bool down) {
        ++counters.fft_searches;
        work.assign(2 * h.n, C{});
        double energy = 0;
        for (size_t i = 0; i < h.n; ++i) {
            const C x = sample(end - h.n + i);
            energy += std::norm(x);
            work[i] = x * (down ? h.up[i] : std::conj(h.up[i]));
        }
        if (energy < 1e-20) return {};
        fft_plan(work.size()).execute(work);
        spectral_prefix.resize(work.size() + 1); spectral_prefix[0] = 0;
        for (size_t i = 0; i < work.size(); ++i)
            spectral_prefix[i + 1] = spectral_prefix[i] + std::norm(work[i]);
        std::vector<Tone> result;
        result.reserve(max_components);
        size_t qualified = 0;
        for (size_t peak = 0; peak < work.size(); ++peak) {
            const double p = std::norm(work[peak]);
            if (p <= std::norm(work[(peak + work.size() - 1) % work.size()]) ||
                p < std::norm(work[(peak + 1) % work.size()])) continue;
            const double background = local_background(peak);
            if (p < 20 * background || p < 1e-20) continue;
            ++qualified;
            if (result.size() == max_components && p <= result.back().power) continue;
            // Log-quadratic interpolation of the local peak. Neither its power
            // nor its ability to dominate other transmitters is an identity test.
            const auto power = [&](size_t i) { return std::log(std::max(1e-30, static_cast<double>(std::norm(work[i])))); };
            const double l = power((peak + work.size() - 1) % work.size());
            const double m = power(peak), r = power((peak + 1) % work.size());
            const double denominator = l - 2 * m + r;
            const double delta = std::abs(denominator) > 1e-12 ? std::clamp(.5 * (l - r) / denominator, -.5, .5) : 0;
            double k = static_cast<double>(peak) + delta;
            if (k >= static_cast<double>(h.n)) k -= static_cast<double>(work.size());
            Tone candidate{k * rate / static_cast<double>(work.size()),
                           p / (static_cast<double>(h.n) * energy), p, background};
            const auto at = std::lower_bound(result.begin(), result.end(), p,
                [](const Tone& value, double power) { return value.power > power; });
            result.insert(at, candidate);
            if (result.size() > max_components) result.pop_back();
        }
        if (qualified > max_components) ++counters.candidate_limit_hits;
        return result;
    }
    // Retain distinct measured reset modes; a nearby interferer can move the
    // single largest coherent sum between nearly equal supported maxima. The
    // delimiter may choose only among these independently verified modes.
    static bool retain_wrap(WrapFits& fits, const WrapFit& candidate, const Hypothesis& h) {
        const double separation = .5 * rate / static_cast<double>(h.n);
        for (size_t i = 0; i < fits.size; ++i) {
            if (std::abs(fits.values[i].center - candidate.center) >= separation) continue;
            if (candidate.match > fits.values[i].match) {
                fits.values[i] = candidate;
                std::sort(fits.values.begin(), fits.values.begin() + fits.size,
                    [](const auto& a, const auto& b) { return a.match > b.match; });
            }
            return false;
        }
        const bool saturated = fits.size == fits.values.size();
        if (saturated && candidate.match <= fits.values[fits.size - 1].match) return true;
        if (!saturated) ++fits.size;
        fits.values[fits.size - 1] = candidate;
        std::sort(fits.values.begin(), fits.values.begin() + fits.size,
            [](const auto& a, const auto& b) { return a.match > b.match; });
        return saturated;
    }
    WrapFits fit_up_wrap(const Hypothesis& h, double dominant_hz) {
        // Independently locate the frequency reset in an observed upchirp.
        // A tone pair or up/down average alone leaves a CFO/timing ambiguity.
        // For reset a, high tone precedes a and low tone follows it; continuity
        // requires exp(-j*2*pi*BW*a/Fs) between their coherent partial sums.
        prefix_high.resize(h.n + 1); prefix_low.resize(h.n + 1);
        double energy = 0;
        for (size_t i = 0; i < h.n; ++i) energy += std::norm(sample(end - h.n + i));
        WrapFits best;
        bool saturated = false;
        if (energy < 1e-20) return best;
        for (unsigned branch = 0; branch < 2; ++branch) {
            const double high_hz = dominant_hz + branch * h.bw;
            const double noise_variance = std::max(background_at(high_hz), background_at(high_hz - h.bw)) /
                                          static_cast<double>(h.n);
            const auto high_step = std::polar(1., -2 * std::numbers::pi * high_hz / rate);
            const auto low_step = std::polar(1., -2 * std::numbers::pi * (high_hz - h.bw) / rate);
            std::complex<double> high_phase{1, 0}, low_phase{1, 0};
            prefix_high[0] = prefix_low[0] = {};
            for (size_t i = 0; i < h.n; ++i) {
                const auto y = std::complex<double>(sample(end - h.n + i) * std::conj(h.up[i]));
                prefix_high[i + 1] = prefix_high[i] + y * high_phase;
                prefix_low[i + 1] = prefix_low[i] + y * low_phase;
                high_phase *= high_step; low_phase *= low_step;
            }
            const size_t period = static_cast<size_t>(rate / h.bw);
            std::array<std::complex<double>, 64> continuity{};
            for (size_t k = 0; k < 4 * period; ++k)
                continuity[k] = std::polar(1., -2 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(4 * period));
            // Quarter-symbol search phases ensure a complete repeated chirp has
            // windows with a supported interior reset. A boundary-only fit can
            // be a single tone or an equal-slope fragment, so cannot arm a lane.
            const auto at_reset = [&](size_t k) -> WrapFit {
                const double a = static_cast<double>(k) * .25;
                const size_t edge = (k + 3) / 4;
                const auto high = prefix_high[edge];
                const auto low = prefix_low[h.n] - prefix_low[edge];
                const double high_magnitude = std::abs(high), low_magnitude = std::abs(low);
                if (std::norm(high) < 20 * static_cast<double>(edge) * noise_variance ||
                    std::norm(low) < 20 * static_cast<double>(h.n - edge) * noise_variance) return {};
                const double high_amplitude = high_magnitude / static_cast<double>(edge);
                const double low_amplitude = low_magnitude / static_cast<double>(h.n - edge);
                if (high_amplitude > 2 * low_amplitude || low_amplitude > 2 * high_amplitude) return {};
                const auto sum = high + continuity[k % (4 * period)] * low;
                const double component_sum = high_magnitude + low_magnitude;
                if (std::norm(sum) < .8 * component_sum * component_sum) return {};
                const double match = std::norm(sum) / (static_cast<double>(h.n) * energy);
                const double center_offset = high_hz - h.bw + h.bw * a / static_cast<double>(h.n);
                double delta = static_cast<double>(h.n) - a;
                if (delta > .5 * static_cast<double>(h.n)) delta -= static_cast<double>(h.n);
                if (std::abs(center_offset) + .5 * h.bw > 750000 ||
                    std::abs(dominant_hz - center_offset - h.bw * delta / static_cast<double>(h.n)) >
                        2 * rate / static_cast<double>(h.n)) return {};
                return {center_offset, delta, match,
                    std::norm(sum) / (static_cast<double>(h.n) * static_cast<double>(h.n))};
            };
            WrapFit previous;
            double left_match = 0;
            const size_t last = 16 * h.n / 5;
            for (size_t k = (4 * h.n + 4) / 5; k <= last + 1; ++k) {
                const auto current = k <= last ? at_reset(k) : WrapFit{};
                // Count supported local maxima, not adjacent quarter-sample
                // grid points representing the same measured reset mode.
                if (previous.match > left_match && previous.match >= current.match)
                    saturated |= retain_wrap(best, previous, h);
                left_match = previous.match;
                previous = current;
            }
        }
        if (saturated) ++counters.candidate_limit_hits;
        return best;
    }
    // Full wrapped-chirp coherent match, after independent up/down estimates
    // resolve RF center and timing. A single dominant FFT peak is insufficient.
    struct TemplateMatch { double match = 0, component_power = 0; };
    TemplateMatch template_match(const Hypothesis& h, uint64_t window_end, double offset,
                                double delta_samples, bool down) const {
        std::complex<double> sum{};
        double energy = 0;
        const double n = static_cast<double>(h.n);
        const double duration = n / rate;
        for (size_t i = 0; i < h.n; ++i) {
            const double t = static_cast<double>(i) / rate;
            double u = std::fmod(static_cast<double>(i) + delta_samples, n);
            if (u < 0) u += n;
            u /= rate;
            double cycles = -.5 * h.bw * u + .5 * h.bw / duration * u * u;
            if (down) cycles = -cycles;
            cycles += offset * t;
            const double a = -2 * std::numbers::pi * std::remainder(cycles, 1.);
            const C x = sample(window_end - h.n + i);
            sum += std::complex<double>(x) * std::complex<double>(std::cos(a), std::sin(a));
            energy += std::norm(x);
        }
        return energy > 1e-20 ? TemplateMatch{std::norm(sum) / (n * energy), std::norm(sum) / (n * n)} : TemplateMatch{};
    }
    void check_pending(const Hypothesis& h, Chain& c, const Callback& callback) {
        if (!c.pending) return;
        const double start_sample = c.pending_found.delimiter_sample;
        if (!std::isfinite(start_sample) || start_sample < static_cast<double>(begin)) {
            c.pending = false; return;
        }
        const auto start = static_cast<uint64_t>(std::floor(start_sample));
        if (start > end || end - start < 2 * h.n) return;
        c.pending = false;
        if (end - start > capacity) return;
        const double delta = static_cast<double>(start) - start_sample;
        const double offset = c.pending_found.center_hz - center;
        const auto first = template_match(h, start + h.n, offset, delta, true);
        const auto second = template_match(h, start + 2 * h.n, offset, delta, true);
        const double square_n = static_cast<double>(h.n) * static_cast<double>(h.n);
        // Verify two complete symbols on the inferred reset grid. Two partial
        // quarter-phase windows can each look strong enough, yet manufacture a
        // second delimiter one symbol late from the same 2.25-symbol SFD.
        if (!std::isfinite(first.component_power) || !std::isfinite(second.component_power) ||
            std::min(first.component_power, second.component_power) < .35 * c.pending_component_power ||
            first.component_power * square_n < 20 * c.pending_first_background ||
            second.component_power * square_n < 20 * c.pending_second_background) return;
        auto found = c.pending_found;
        found.down_match = std::min(first.match, second.match);
        const double tolerance = 2. * rate / static_cast<double>(h.n);
        const bool duplicate = std::any_of(recent.begin(), recent.end(), [&](const auto& previous) {
            return previous.bandwidth_hz == found.bandwidth_hz && previous.spreading_factor == found.spreading_factor &&
                std::abs(previous.center_hz - found.center_hz) < tolerance &&
                std::abs(previous.delimiter_sample - found.delimiter_sample) < static_cast<double>(h.n) * .5;
        });
        c = {};
        if (!duplicate) {
            ++counters.confirmations;
            if (recent.size() == 128) recent.erase(recent.begin());
            recent.push_back(found); callback(found);
        }
    }
    void check_delimiter(const Hypothesis& h, Chain& c, const std::vector<Tone>& downs,
                         const Callback& callback) {
        if (!c.armed || c.pending || end - c.armed_end <= h.n) return;
        const double tolerance = 2. * rate / static_cast<double>(h.n);
        const Tone* candidate = nullptr;
        double candidate_distance = 2 * tolerance;
        for (size_t i = 0; i < c.reset_fits.size; ++i) {
            const double expected = 2 * c.reset_fits.values[i].center - c.armed_hz;
            for (const auto& tone : downs) {
                const double distance = std::abs(tone.hz - expected);
                if (distance <= candidate_distance) { candidate = &tone; candidate_distance = distance; }
            }
        }
        if (!candidate) { c.down_count = 0; return; }
        const auto& down = *candidate;
        const auto retain_current = [&] {
            c.down_count = 1; c.down_hz = down.hz; c.down_match = down.match;
            c.down_background = down.background; c.down_end = end;
        };
        if (!c.down_count || end - c.down_end != h.n || std::abs(down.hz - c.down_hz) > tolerance) {
            retain_current(); return;
        }
        const double down_hz = .5 * (down.hz + c.down_hz);
        const double offset = .5 * (c.armed_hz + down_hz);
        const double delta = .5 * (c.armed_hz - down_hz) * static_cast<double>(h.n) / h.bw;
        // Independently fitted preamble reset evidence prevents a carrier step
        // at the SFD from manufacturing a center between two different signals.
        const WrapFit* supported = nullptr;
        for (size_t i = 0; i < c.reset_fits.size; ++i) {
            const auto& fit = c.reset_fits.values[i];
            if (std::abs(offset - fit.center) <= tolerance && std::abs(delta - fit.delta) <= 2 * rate / h.bw &&
                (!supported || fit.match > supported->match)) supported = &fit;
        }
        if (!supported || std::abs(delta) > .5 * static_cast<double>(h.n) ||
            std::abs(offset) + .5 * h.bw > 750000) { retain_current(); return; }
        double observed_up = static_cast<double>(c.armed_first) - delta;
        // Select the fitted reset inside the first observed window. The
        // preceding periodic reset may be before the signal actually arrived.
        if (observed_up < static_cast<double>(c.armed_first)) observed_up += static_cast<double>(h.n);
        c.pending_found = {center + offset, h.bw, h.sf,
            observed_up,
            static_cast<double>(end - 2 * h.n) - delta,
            c.armed_match, 0, c.armed_coherence};
        c.pending_component_power = supported->component_power;
        c.pending_first_background = c.down_background;
        c.pending_second_background = down.background;
        c.pending = true;
        retain_current();
        check_pending(h, c, callback);
    }
    void evaluate(size_t index, double repeat, const Callback& callback) {
        const auto& h = bank()[index];
        // Pending candidates already have their search evidence. Confirm each
        // as soon as both complete SFD symbols are available; waiting for its
        // original search phase can require an unnecessary extra symbol tail.
        for (auto& pending_phase : chains[index])
            for (auto& c : pending_phase) check_pending(h, c, callback);
        auto& phase = chains[index][(end / (h.n / 4)) % 4];
        const double tolerance = 2. * rate / static_cast<double>(h.n);
        bool armed = false;
        for (auto& c : phase) {
            if (c.armed && end - c.armed_end > 6 * h.n) c = {};
            if (!c.armed && c.streak && end - c.last_up_end > h.n) c = {};
            armed |= c.armed;
        }
        if (!armed && !chirp_screen(h)) {
            for (auto& c : phase) c = {};
            return;
        }
        ++counters.repeat_gates;
        const auto ups = tones(h, false);
        std::array<bool, max_components> updated{};
        for (const auto& up : ups) {
            size_t slot = max_components;
            double distance = tolerance;
            for (size_t i = 0; i < phase.size(); ++i) {
                const auto& c = phase[i];
                if (updated[i] || !c.streak || end - c.last_up_end != h.n) continue;
                const double difference = std::abs(up.hz - c.last_up_hz);
                if (difference <= distance) { slot = i; distance = difference; }
            }
            if (slot == max_components) {
                for (size_t i = 0; i < phase.size(); ++i)
                    if (!updated[i] && !phase[i].armed && !phase[i].streak) { slot = i; break; }
                if (slot == max_components) { ++counters.track_limit_hits; continue; }
                phase[slot] = {};
                phase[slot].streak_start = end - h.n;
            }
            auto& c = phase[slot];
            updated[slot] = true;
            if (c.streak < 4) { ++c.streak; c.sum_up_hz += up.hz; }
            else c.sum_up_hz += up.hz - c.sum_up_hz / 4.;
            c.up_match = std::max(c.up_match, up.match);
            c.up_coherence = std::max(c.up_coherence, repeat);
            c.last_up_hz = up.hz; c.last_up_end = end;
            if (c.streak >= 4) {
                const auto fits = fit_up_wrap(h, up.hz);
                if (!fits.size) continue;
                bool saturated = false;
                for (size_t i = 0; i < fits.size; ++i) saturated |= retain_wrap(c.reset_fits, fits.values[i], h);
                if (saturated) ++counters.candidate_limit_hits;
                c.armed = true; c.armed_hz = c.sum_up_hz / 4.;
                c.armed_match = c.up_match; c.armed_coherence = c.up_coherence;
                c.armed_first = c.streak_start; c.armed_end = end;
                // Every window in this phase advances by exactly one symbol,
                // so a verified reset's local delta remains on the same grid.
                // Keep the strongest supported alternatives when a later fit
                // is pulled toward an interferer. Timeout still follows current
                // observed upchirps, within this continuous component streak.
            }
        }
        for (size_t i = 0; i < phase.size(); ++i)
            if (!updated[i]) phase[i].streak = 0;
        const bool search_down = std::any_of(phase.begin(), phase.end(), [&](const auto& c) {
            return c.armed && end - c.armed_end > h.n;
        });
        if (!search_down) return;
        const auto downs = tones(h, true);
        for (auto& c : phase) check_delimiter(h, c, downs, callback);
    }
    void checkpoint(const Callback& callback) {
        std::array<double, 8> repeats{};
        std::array<bool, 8> computed{};
        for (size_t i = 0; i < bank().size(); ++i) {
            const auto n = bank()[i].n;
            if (end - begin < 2 * n || end % (n / 4)) continue;
            ++counters.windows;
            unsigned period = 0;
            for (size_t d = 512; d < n; d *= 2) ++period;
            if (!computed[period]) { repeats[period] = coherence(n); computed[period] = true; }
            evaluate(i, repeats[period], callback);
        }
    }
};
LoRaPreambleDiscovery::LoRaPreambleDiscovery(double center) : impl_(std::make_unique<Impl>(center)) {}
LoRaPreambleDiscovery::~LoRaPreambleDiscovery() { reset(); }
void LoRaPreambleDiscovery::feed(std::span<const C> samples, uint64_t first, const Callback& callback) {
    auto& p = *impl_;
    if (samples.size() > std::numeric_limits<uint64_t>::max() - first) throw std::invalid_argument("Discovery sample index overflow");
    constexpr uint64_t exact_coordinate_limit = uint64_t{1} << 53;
    if (first > exact_coordinate_limit || samples.size() > exact_coordinate_limit - first)
        throw std::invalid_argument("Discovery sample index exceeds exact timestamp domain");
    if (!p.initialized || first != p.end) {
        if (p.initialized) ++p.counters.resets;
        p.begin = p.end = first; p.initialized = true;
        p.repetition.reset();
        p.screen.reset();
        for (auto& phases : p.chains) phases = {};
        p.recent.clear();
    }
    for (const auto x : samples) {
        if (!std::isfinite(x.real()) || !std::isfinite(x.imag()) || std::abs(x.real()) > 16 || std::abs(x.imag()) > 16) {
            ++p.end; ++p.counters.samples; ++p.counters.resets; p.begin = p.end;
            p.repetition.reset();
            p.screen.reset();
            for (auto& phases : p.chains) phases = {};
            p.recent.clear();
            continue;
        }
        p.ring[p.end % capacity] = x;
        p.repetition.push(x);
        p.screen.push(x, p.end);
        ++p.end; ++p.counters.samples;
        if (p.end % 128 == 0) p.checkpoint(callback);
    }
}
void LoRaPreambleDiscovery::reset() {
    auto& p = *impl_;
    p.initialized = false; ++p.counters.resets;
    p.repetition.reset();
    p.screen.reset();
    std::fill(p.ring.begin(), p.ring.end(), C{});
    std::fill(p.work.begin(), p.work.end(), C{});
    std::fill(p.prefix_high.begin(), p.prefix_high.end(), std::complex<double>{});
    std::fill(p.prefix_low.begin(), p.prefix_low.end(), std::complex<double>{});
    for (auto& phases : p.chains) phases = {};
    p.recent.clear();
}
LoRaDiscoveryStats LoRaPreambleDiscovery::stats() const { return impl_->counters; }
}
