#pragma once

#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ovmesh {

constexpr bool supported_lora_bandwidth(std::uint32_t bandwidth) noexcept {
    return bandwidth == 15625 || bandwidth == 62500 || bandwidth == 125000 ||
           bandwidth == 250000 || bandwidth == 500000;
}

// Forward transform is unnormalized; inverse divides by N. N must be a power of two.
void fft_inplace(std::span<std::complex<float>> values, bool inverse = false);

struct PhyConfig {
    std::uint32_t bandwidth_hz = 250000;
    std::uint8_t spreading_factor = 11;
    // Denominator of 4/5 through 4/8. Explicit headers supply the received rate.
    std::uint8_t coding_rate = 5;
    std::uint8_t sync_word = 0x2b;
};

struct PhyFrame {
    // Transient RF frame bytes, never a persistence record. No authentication is implied.
    std::vector<std::uint8_t> bytes;
    bool header_valid = false;
    bool payload_crc_valid = false;
    bool payload_crc_present = false;
    bool fec_corrected = false;
    std::uint8_t coding_rate = 0;
    // Coarse start of the observed preamble (up to one symbol of acquisition uncertainty).
    // This is a host-stream sample index, not a hardware RF timestamp.
    std::uint64_t first_sample = 0;
    // Exclusive end, relative to feed() samples since construction/reset().
    std::uint64_t last_sample = 0;
    float snr_db = 0;
    float frequency_error_hz = 0;
};

// Aggregate acquisition diagnostics contain no samples, symbols, or payloads.
// Counts saturate at UINT64_MAX, survive reset(), and start at zero on construction.
struct PhyDiagnostics {
    std::uint64_t preamble_candidates = 0;
    std::uint64_t sync_matches = 0;
    std::uint64_t sync_rejections = 0;
    std::uint64_t sync_low_ratio = 0;
    std::uint64_t sync_timeout = 0;
    std::uint64_t sync_first_mismatch = 0;
    std::uint64_t sync_second_mismatch = 0;
    std::uint64_t headers_valid = 0;
    std::uint64_t headers_failed = 0;
    // Includes completed frames with a failed payload CRC; no protocol identity implied.
    std::uint64_t completed_frames = 0;
};

// Single-thread-owned receiver. Inputs must be centered and sampled at bandwidth_hz.
// Explicit headers, normal chirp polarity, and a configured sync word are supported.
// reset() is required after a sample gap, retune, rate change, or source change.
class LoRaReceiver {
public:
    explicit LoRaReceiver(PhyConfig config);
    ~LoRaReceiver();
    LoRaReceiver(LoRaReceiver&&) noexcept;
    LoRaReceiver& operator=(LoRaReceiver&&) noexcept;
    LoRaReceiver(const LoRaReceiver&) = delete;
    LoRaReceiver& operator=(const LoRaReceiver&) = delete;
    void feed(std::span<const std::complex<float>> samples,
              const std::function<void(PhyFrame&&)>& on_frame);
    void reset();
    [[nodiscard]] PhyDiagnostics diagnostics() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Native symbol codec: each input is an unshifted dechirped FFT peak in [0, 2^SF).
// nullopt means incomplete or structurally invalid; CRC failure is a returned status.
std::optional<PhyFrame> decode_lora_symbols(std::span<const std::uint16_t> bins,
                                           PhyConfig config);

// In-memory synthetic fixtures only. There is no device or RF-transmit operation.
// Payload length is 2..255 bytes; explicit header and payload CRC are enabled.
std::vector<std::uint16_t> encode_lora_symbols(std::span<const std::uint8_t> payload,
                                             PhyConfig config);
// Omitted/zero rate preserves the historical chip-rate fixture convention.
// An explicit rate generates phase-continuous analytic chirps directly at that
// rate; no chip holds/interpolation. It must be an integer multiple of bandwidth,
// at most 20 MS/s, with at most 16,777,216 complex output samples (128 MiB).
std::vector<std::complex<float>> modulate_lora(std::span<const std::uint8_t> payload,
                                              PhyConfig config,
                                              std::uint32_t sample_rate_hz = 0);

} // namespace ovmesh
