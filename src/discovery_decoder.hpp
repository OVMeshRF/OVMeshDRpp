// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "discovery_channelizer.hpp"
#include "lora_discovery.hpp"
#include "ovmesh/phy.hpp"
#include <memory>

namespace ovmesh {

struct DiscoveryDecoderOptions {
    struct Exclusion {
        double center_hz = 0;
        uint32_t bandwidth_hz = 0;
        unsigned spreading_factor = 0;
    };
    bool enabled = false;
    std::vector<Exclusion> exclusions;
};

// Owns transient received bytes. Destruction and replacement erase them; this
// type cannot be copied into a metadata record accidentally. No keys or sender
// identities belong to this handoff. CRC-failed frames carry metadata only.
struct DiscoveryDecodedFrame {
    size_t subband_index = 0;
    uint64_t segment_id = 0;
    double center_hz = 0;
    uint32_t bandwidth_hz = 0;
    unsigned spreading_factor = 0;
    double first_input_sample = 0, end_input_sample = 0, delimiter_input_sample = 0;
    PhyFrame frame;
    DiscoveryDecodedFrame() = default;
    ~DiscoveryDecodedFrame();
    DiscoveryDecodedFrame(DiscoveryDecodedFrame&&) noexcept;
    DiscoveryDecodedFrame& operator=(DiscoveryDecodedFrame&&) noexcept;
    DiscoveryDecodedFrame(const DiscoveryDecodedFrame&) = delete;
    DiscoveryDecodedFrame& operator=(const DiscoveryDecodedFrame&) = delete;
};

struct DiscoveryDecoderStats {
    uint64_t candidates = 0, started = 0, completed = 0, crc_valid = 0;
    uint64_t history_misses = 0, active_limit_hits = 0, duplicate_candidates = 0;
    uint64_t excluded_candidates = 0, outside_range_candidates = 0, unsupported_candidates = 0;
    uint64_t timeouts = 0, resets = 0, abandoned_decoders = 0, frame_overflows = 0;
    size_t active_decoders = 0, history_samples = 0;
    PhyDiagnostics phy;
};

// Single detector-thread-owned dispatch. Feed a bounded batch into history
// BEFORE reporting its discoveries. A confirmed candidate can then replay its
// preamble and the rest of that batch exactly once, followed by future input.
// IQ, symbols and frame bytes remain transient; no filesystem or keys are used.
class DiscoveryDecoder {
public:
    static constexpr size_t maximum_block = 4096;
    static constexpr size_t maximum_active = 4;
    static constexpr size_t maximum_exclusions = 16;
    static constexpr size_t maximum_history_samples = 2 * 1024 * 1024;
    static constexpr size_t maximum_total_history_samples = 64 * 1024 * 1024;
    static constexpr size_t recent_capacity = 64;
    using Callback = std::function<void(DiscoveryDecodedFrame&&)>;

    DiscoveryDecoder(size_t subband_index,
                     std::span<const DiscoveryChannelizer::Subband> bands,
                     double requested_lower_hz, double requested_upper_hz,
                     const DiscoveryDecoderOptions& options,
                     size_t history_capacity = maximum_history_samples);
    ~DiscoveryDecoder();
    DiscoveryDecoder(const DiscoveryDecoder&) = delete;
    DiscoveryDecoder& operator=(const DiscoveryDecoder&) = delete;
    void feed(std::span<const std::complex<float>> samples, uint64_t first_local_sample,
              uint64_t input_anchor, uint32_t input_stride, uint64_t segment_id,
              const Callback& callback);
    void observe(const LoRaDiscovery& found, const Callback& callback);
    void reset();
    [[nodiscard]] DiscoveryDecoderStats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ovmesh
