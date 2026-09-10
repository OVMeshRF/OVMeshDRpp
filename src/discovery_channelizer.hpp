// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace ovmesh {

// Shared, twice-oversampled polyphase analysis bank. Unlike a chip-rate LoRa
// decoder, each output preserves a complete off-center 500 kHz chirp: subband
// centers are 1 MHz apart, outputs are 2 MS/s, and the declared flat passband is
// +/-750 kHz. Receiver RF flatness is a separate, uncalibrated property.
// This class performs channelization only; it does not identify any protocol.
class DiscoveryChannelizer {
public:
    static constexpr uint32_t output_sample_rate = 2000000;
    static constexpr uint32_t center_spacing_hz = 1000000;
    static constexpr uint32_t passband_half_width_hz = 750000;
    static constexpr size_t taps_per_phase = 32;

    struct Subband {
        double center_hz = 0;
        double passband_lower_hz = 0;
        double passband_upper_hz = 0;
    };
    struct Samples {
        size_t subband_index = 0;
        // The center of the first FIR output, after its exact group delay is
        // removed, in the caller's input-sample coordinate. Subsequent samples
        // advance input_sample_stride. Warmup outputs are never delivered.
        uint64_t first_input_sample = 0;
        uint32_t input_sample_stride = 0;
        // Borrowed storage: valid only until this callback returns.
        std::span<const std::complex<float>> values;
    };
    using Callback = std::function<void(const Samples&)>;

    DiscoveryChannelizer(uint32_t input_sample_rate, double receiver_center_hz,
                         double requested_lower_hz, double requested_upper_hz);
    ~DiscoveryChannelizer();
    DiscoveryChannelizer(DiscoveryChannelizer&&) noexcept;
    DiscoveryChannelizer& operator=(DiscoveryChannelizer&&) noexcept;
    DiscoveryChannelizer(const DiscoveryChannelizer&) = delete;
    DiscoveryChannelizer& operator=(const DiscoveryChannelizer&) = delete;

    [[nodiscard]] const std::vector<Subband>& subbands() const noexcept;
    [[nodiscard]] uint32_t input_sample_rate() const noexcept;
    [[nodiscard]] uint32_t group_delay_input_samples() const noexcept;
    [[nodiscard]] size_t prototype_tap_count() const noexcept;
    void feed(std::span<const std::complex<float>> input,
              uint64_t first_input_sample, const Callback& callback);
    // Required before a gap, retune or sample origin change. Clears filter state.
    void reset();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ovmesh
