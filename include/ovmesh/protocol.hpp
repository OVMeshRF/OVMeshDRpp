// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ovmesh::protocol {

// Move-only key material. No implicit/default keys and no persistence methods.
class ChannelKey {
public:
    static std::optional<ChannelKey> from_hex(std::string_view hex);
    // Explicit input only: 32/64 hex characters, canonical padded standard
    // Base64 for 16/32 bytes, or exactly AQ== (Meshtastic public key index 1).
    // Rejects AA== (encryption disabled), other short indices, whitespace,
    // omitted padding, URL-safe Base64, and nonzero unused padding bits.
    // The caller owns and must clear the original input string after use.
    static std::optional<ChannelKey> from_user_input(std::string_view input);
    ChannelKey(ChannelKey&& other) noexcept;
    ChannelKey& operator=(ChannelKey&& other) noexcept;
    ChannelKey(const ChannelKey&) = delete;
    ChannelKey& operator=(const ChannelKey&) = delete;
    ~ChannelKey();
    std::span<const std::uint8_t> bytes() const noexcept;
private:
    ChannelKey() = default;
    std::array<std::uint8_t, 32> data_{};
    std::size_t size_{};
};

struct Profile {
    std::string id;
    std::string label;
    // Resolved, case-sensitive firmware channel name (for example LongFast).
    // Optional when restrict_channel_name is explicitly false.
    std::string channel_name;
    std::vector<ChannelKey> keys;
    // Explicit key-only survey records disable name/hash narrowing. The default
    // preserves existing name-restricted profile behavior; no keys are inferred.
    bool restrict_channel_name{true};
};

inline constexpr std::size_t max_keyring_profiles = 16;
inline constexpr std::size_t max_profile_keys = 16;

enum class Status {
    decoded, bad_phy_crc, invalid_frame, no_matching_key,
    decryption_unvalidated, unsupported_payload, ambiguous_keys,
    unsupported_pki, invalid_profile, crypto_error
};
std::string_view status_name(Status status) noexcept;

// Only explicitly supported schema fields can leave the decoder. All positions
// here are sender-reported and must never be confused with receiver GPS.
struct DecodedContent {
    std::string kind;
    std::string text;
    std::string node_id;
    std::string long_name;
    std::string short_name;
    std::optional<double> latitude, longitude, altitude;
    std::optional<double> voltage, temperature, humidity, battery_percent;
    std::optional<double> channel_utilization, air_util_tx;
    std::optional<std::uint32_t> reported_time, hardware_model, role, routing_error;
    std::vector<std::uint32_t> route;
    std::vector<std::uint32_t> route_back;
    // Wire values are dB multiplied by four; preserve missing entries and
    // sentinels rather than inventing one-to-one alignment with route nodes.
    std::vector<std::int32_t> snr_towards, snr_back;
    // Routing oneof only: request, reply, error. Empty for direct port 70.
    std::string routing_variant;
};

struct AuthorizedContent {
    std::string profile_id;
    std::uint32_t from{}, to{}, packet_id{}, port{};
    std::uint8_t hop_limit{}, hop_start{}, channel_hash{}, next_hop{}, relay_node{};
    bool want_ack{}, via_mqtt{}, want_response{};
    std::uint32_t request_id{}, reply_id{};
    bool signature_present{}; // Presence only; this decoder does not verify it.
    DecodedContent content;
};

// Minimal evidence from a bounded Data envelope with an explicitly configured
// matching channel-key attempt. No identifiers or unsupported payload bytes.
// AES-CTR is unauthenticated; envelope plausibility is not cryptographic proof.
struct EnvelopeEvidence {
    std::uint32_t port{};
    bool signature_present{};
};

struct DecodeResult {
    Status status{Status::invalid_frame};
    std::string classification{"unknown LoRa"};
    // Meshtastic channel AES-CTR has no integrity tag. Even accepted decryption
    // and a strict schema projection cannot authenticate the packet's source.
    std::string authentication{"not authenticated"};
    std::optional<EnvelopeEvidence> evidence;
    std::optional<AuthorizedContent> authorized;
};

std::uint8_t channel_hash(std::string_view channel_name, const ChannelKey& key) noexcept;
std::array<std::uint8_t, 16> meshtastic_nonce(std::uint32_t from, std::uint32_t packet_id) noexcept;
DecodeResult decode_meshtastic(std::span<const std::uint8_t> frame,
                              bool phy_crc_valid, const Profile& profile);
// A keyring is independent of receiver frequencies and discovery lanes. Only
// explicitly supplied keys are attempted. Name-restricted profiles require a
// matching header hash; explicit key-only profiles try across channel hashes.
// All records are validated before decoding. An empty keyring has no
// implicit fallback. At most max_keyring_profiles * max_profile_keys attempts.
// Competing plausible envelopes from distinct keys suppress content/evidence.
// Identical eligible key material is one candidate; differing profile IDs use
// the fixed provenance "configured-keyring" without inferring a channel name.
// profile_id identifies the user's configuration, never the sender.
DecodeResult decode_meshtastic(std::span<const std::uint8_t> frame,
                              bool phy_crc_valid, std::span<const Profile> profiles);

// Generates synthetic bytes in memory for the receive pipeline only. There is
// no radio/TX API. The demo key is artificial and only this function installs it.
Profile synthetic_profile();
std::vector<std::uint8_t> synthetic_text_frame(const Profile& profile,
                                             std::uint32_t packet_id = 1,
                                             std::string_view text = "Synthetic survey packet");

} // namespace ovmesh::protocol
