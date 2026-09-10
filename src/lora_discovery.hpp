// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace ovmesh {

// Waveform evidence only: repeated upchirps followed by two downchirps.
// Neither a CRC-validated frame nor a Meshtastic/MeshCore identification.
struct LoRaDiscovery {
    double center_hz = 0;
    uint32_t bandwidth_hz = 0;
    unsigned spreading_factor = 0;
    // First confirmed repeated-upchirp window; acquisition can begin after the
    // transmitter's actual preamble start. This is not a packet airtime edge.
    double first_observed_upchirp_sample = 0;
    double delimiter_sample = 0;
    // Fractions of total subband power matching these observations, not
    // probabilities or protocol confidence. Other signals can lower them.
    double up_match = 0, down_match = 0;
    // Aggregate subband diagnostic only: simultaneous components can cancel
    // this complex correlation despite valid individual repeated preambles.
    double repeat_coherence = 0;
};

struct LoRaDiscoveryStats {
    uint64_t samples = 0, windows = 0, fft_searches = 0;
    // repeat_gates retains its experimental API name; it counts windows that
    // reached component search after a slope screen or an already armed track.
    uint64_t repeat_gates = 0, confirmations = 0, resets = 0;
    // A transform or reset fit found more than eight qualified candidates, or
    // a qualified peak could not receive one of eight per-hypothesis/per-phase tracks. These
    // count bounded-search pressure, not known packets or confirmed losses.
    uint64_t candidate_limit_hits = 0, track_limit_hits = 0;
};

// Bounded, streaming 2 MS/s complex-IQ experiment. Searches all 18 hypotheses
// (125/250/500 kHz, SF7..12); the caller supplies no candidate RF settings.
// At most eight spectral candidates and eight independent component tracks
// per hypothesis/search phase are considered; limits are exposed in stats().
// Each track retains at most eight independently supported preamble reset fits.
// Input must preserve the complete chirp with an adequate anti-alias guard.
// Input sample coordinates and their exclusive end must not exceed 2^53, the
// exact-integer domain used by the fractional timestamp calculations.
// No IQ or payload bytes are emitted or written to files. Transient buffers do
// not establish guarantees about OS swap or crash dumps. CPU capacity and RF
// sensitivity require separate validation before enabling a live survey path.
class LoRaPreambleDiscovery {
public:
    using Callback = std::function<void(const LoRaDiscovery&)>;
    explicit LoRaPreambleDiscovery(double subband_center_hz);
    ~LoRaPreambleDiscovery();
    LoRaPreambleDiscovery(const LoRaPreambleDiscovery&) = delete;
    LoRaPreambleDiscovery& operator=(const LoRaPreambleDiscovery&) = delete;
    void feed(std::span<const std::complex<float>>, uint64_t first_sample, const Callback&);
    void reset();
    LoRaDiscoveryStats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
