// SPDX-License-Identifier: GPL-3.0-or-later
// Wire behavior follows pinned Meshtastic firmware/protobuf definitions. See
// docs/research/meshtastic-wire-baseline.md. No device APIs or upstream key policy.
#include "ovmesh/protocol.hpp"
#include "protobuf_guard.hpp"
#include <pb_decode.h>
#include <meshtastic/mesh.pb.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/provider.h>

#include <algorithm>
#include <memory>
#include <utility>

namespace ovmesh::protocol {
namespace {
constexpr std::size_t max_frame = 255;
using Bytes = std::span<const std::uint8_t>;

struct ClearedBuffer {
    std::array<std::uint8_t, 256> bytes{};
    ~ClearedBuffer() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
};

std::uint32_t le32(Bytes bytes) {
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
           (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}
void put32(std::uint8_t* p, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// Private provider context: no openssl.cnf or environment-selected provider
// configuration is loaded. The default provider is built into libcrypto.
struct CryptoContext {
    OSSL_LIB_CTX* library{OSSL_LIB_CTX_new()};
    OSSL_PROVIDER* provider{library ? OSSL_PROVIDER_load(library, "default") : nullptr};
    EVP_CIPHER* aes128{provider ? EVP_CIPHER_fetch(library, "AES-128-CTR", "provider=default") : nullptr};
    EVP_CIPHER* aes256{provider ? EVP_CIPHER_fetch(library, "AES-256-CTR", "provider=default") : nullptr};
    ~CryptoContext() {
        EVP_CIPHER_free(aes128); EVP_CIPHER_free(aes256);
        if (provider) OSSL_PROVIDER_unload(provider);
        OSSL_LIB_CTX_free(library);
    }
};

bool crypt(Bytes input, const ChannelKey& key, const std::array<std::uint8_t, 16>& nonce,
           std::span<std::uint8_t> output) {
    if (input.size() > max_frame || output.size() < input.size()) return false;
    static CryptoContext crypto;
    const auto* cipher = key.bytes().size() == 16 ? crypto.aes128 : crypto.aes256;
    if (!cipher || (key.bytes().size() != 16 && key.bytes().size() != 32)) return false;
    const auto ctx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>(
        EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx || EVP_EncryptInit_ex2(ctx.get(), cipher, key.bytes().data(), nonce.data(), nullptr) != 1) return false;
    int written = 0, final = 0;
    if (EVP_EncryptUpdate(ctx.get(), output.data(), &written, input.data(), static_cast<int>(input.size())) != 1) return false;
    if (EVP_EncryptFinal_ex(ctx.get(), output.data() + written, &final) != 1) return false;
    return written + final == static_cast<int>(input.size());
}

template<class Message> struct ClearedMessage {
    Message value{};
    ~ClearedMessage() { OPENSSL_cleanse(&value, sizeof(value)); }
};
template<class Message>
bool decode_message(Bytes bytes, const pb_msgdesc_t* descriptor, Message& value) {
    if (!detail::validate_wire(bytes, descriptor)) return false;
    auto stream = pb_istream_from_buffer(bytes.data(), bytes.size());
    return pb_decode(&stream, descriptor, &value) && stream.bytes_left == 0;
}
bool parse_data(Bytes bytes, EnvelopeEvidence& out) {
    ClearedMessage<meshtastic_Data> decoded;
    auto& value = decoded.value;
    if (!decode_message(bytes, &meshtastic_Data_msg, value)) return false;
    const auto port = static_cast<std::uint32_t>(value.portnum);
    if (port == 0 || port > 65535 || value.payload.size > 233 ||
        (value.xeddsa_signature.size != 0 && value.xeddsa_signature.size != 64)) return false;
    if (!detail::has_wire_field(bytes, meshtastic_Data_payload_tag) && port != meshtastic_PortNum_TRACEROUTE_APP) return false;
    // Payload stays opaque. Do not parse text, telemetry, node information,
    // positions or routes. Both this bounded envelope and plaintext are cleared.
    out = {port, value.xeddsa_signature.size != 0};
    return true;
}
bool recognized_port(std::uint32_t port) {
    return port == meshtastic_PortNum_TEXT_MESSAGE_APP || port == meshtastic_PortNum_POSITION_APP ||
        port == meshtastic_PortNum_NODEINFO_APP || port == meshtastic_PortNum_ROUTING_APP ||
        port == meshtastic_PortNum_TELEMETRY_APP || port == meshtastic_PortNum_TRACEROUTE_APP;
}
} // namespace

std::optional<ChannelKey> ChannelKey::from_hex(std::string_view hex) {
    if (hex.size() != 32 && hex.size() != 64) return std::nullopt;
    ChannelKey key; key.size_ = hex.size() / 2;
    auto digit = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
    for (std::size_t i = 0; i < key.size_; ++i) {
        const int a = digit(hex[i * 2]), b = digit(hex[i * 2 + 1]);
        if (a < 0 || b < 0) return std::nullopt;
        key.data_[i] = static_cast<std::uint8_t>((a << 4) | b);
    }
    return key;
}
std::optional<ChannelKey> ChannelKey::from_user_input(std::string_view input) {
    if (input.size() == 32 || input.size() == 64) return from_hex(input);
    if (input == "AQ==") {
        // Public index 1 only; the caller selects whether to install this key.
        // Meshtastic firmware 6d41e279f1f51bd59f687b9d441c1bf47b1594fc:
        // src/mesh/Channels.cpp:229-240 expands byte 1 without modification;
        // src/mesh/Channels.h:144-145 defines these published protocol bytes.
        return from_hex("d4f1bb3a20290759f0bcffabcf4e6901");
    }
    const bool aes128 = input.size() == 24;
    if (!aes128 && input.size() != 44) return std::nullopt;
    const std::size_t data_chars = aes128 ? 22 : 43;
    if (input.substr(data_chars) != (aes128 ? "==" : "=")) return std::nullopt;
    auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    // Decode directly into the RAII key: partial input is cleansed on every
    // failure path, and moving the result clears this local copy. This tiny
    // accumulator is also cleared; no temporary decoded byte buffer is used.
    struct Accumulator {
        std::uint32_t value{};
        ~Accumulator() { OPENSSL_cleanse(&value, sizeof(value)); }
    } accumulator;
    ChannelKey key;
    unsigned bits = 0;
    for (std::size_t i = 0; i < data_chars; ++i) {
        const int value = digit(input[i]);
        if (value < 0) return std::nullopt;
        accumulator.value = (accumulator.value << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            key.data_[key.size_++] = static_cast<std::uint8_t>(accumulator.value >> bits);
        }
    }
    // Standard canonical Base64 requires unused low bits in the final sextet
    // to be zero. Lenient decoders otherwise accept multiple spellings.
    if ((accumulator.value & ((std::uint32_t{1} << bits) - 1)) != 0) return std::nullopt;
    return key;
}
ChannelKey::ChannelKey(ChannelKey&& other) noexcept : data_(other.data_), size_(other.size_) {
    OPENSSL_cleanse(other.data_.data(), other.data_.size()); other.size_ = 0;
}
ChannelKey& ChannelKey::operator=(ChannelKey&& other) noexcept {
    if (this != &other) {
        OPENSSL_cleanse(data_.data(), data_.size()); data_ = other.data_; size_ = other.size_;
        OPENSSL_cleanse(other.data_.data(), other.data_.size()); other.size_ = 0;
    }
    return *this;
}
ChannelKey::~ChannelKey() { OPENSSL_cleanse(data_.data(), data_.size()); }
std::span<const std::uint8_t> ChannelKey::bytes() const noexcept { return {data_.data(), size_}; }
std::uint8_t channel_hash(std::string_view name, const ChannelKey& key) noexcept {
    std::uint8_t hash{};
    for (const char c : name) hash ^= static_cast<unsigned char>(c);
    for (const auto c : key.bytes()) hash ^= c;
    return hash;
}
std::array<std::uint8_t, 16> meshtastic_nonce(std::uint32_t from, std::uint32_t id) noexcept {
    std::array<std::uint8_t, 16> nonce{}; put32(nonce.data(), id); put32(nonce.data() + 8, from); return nonce;
}
std::string_view status_name(Status status) noexcept {
    switch (status) {
    case Status::classified: return "envelope validated"; case Status::bad_phy_crc: return "bad phy CRC";
    case Status::invalid_frame: return "invalid frame"; case Status::no_matching_key: return "no matching key";
    case Status::decryption_unvalidated: return "decryption unvalidated";
    case Status::unsupported_payload: return "unsupported payload"; case Status::ambiguous_keys: return "ambiguous keys";
    case Status::unsupported_pki: return "unsupported PKI"; case Status::invalid_profile: return "invalid profile";
    case Status::crypto_error: return "crypto error";
    }
    return "invalid frame";
}

std::string_view status_explanation(Status status) noexcept {
    switch (status) {
    case Status::classified: return "A configured channel key produced a plausible Meshtastic envelope. The source is not authenticated.";
    case Status::bad_phy_crc: return "The received LoRa frame has no passing integrity check, so classification stopped before any channel key was tried. The check failed or was unavailable; keys cannot repair radio bits.";
    case Status::invalid_frame: return "The received bytes do not form a supported Meshtastic radio envelope. They may belong to another protocol or be incomplete.";
    case Status::no_matching_key: return "No configured key was eligible for this frame. Enable the public Meshtastic key or configure an authorized survey key; a name-restricted key may not match this channel.";
    case Status::decryption_unvalidated: return "Configured keys were tried, but none produced a valid Meshtastic envelope. This can be another key, another protocol, or an unsupported frame; it does not identify the cause.";
    case Status::unsupported_payload: return "A configured key produced a plausible Meshtastic envelope with an unrecognized application port. Content is not retained.";
    case Status::ambiguous_keys: return "More than one distinct key produced plausible envelope metadata. Classification is withheld because the result is ambiguous.";
    case Status::unsupported_pki: return "This frame uses the channel-zero unicast convention, which may require recipient public-key encryption. Channel keys alone cannot classify that mode here.";
    case Status::invalid_profile: return "The configured key record is invalid. Check its scope and input in Settings.";
    case Status::crypto_error: return "The local cryptographic operation failed. This is a processing error, not evidence of a different radio protocol.";
    }
    return "Classification is unavailable.";
}

Profile public_meshtastic_profile() {
    Profile profile;
    profile.id = "public-meshtastic";
    profile.label = "Public Meshtastic";
    profile.restrict_channel_name = false;
    profile.keys.push_back(std::move(*ChannelKey::from_user_input("AQ==")));
    return profile;
}

DecodeResult classify_meshtastic(Bytes frame, bool crc, const Profile& profile) {
    return classify_meshtastic(frame, crc, std::span<const Profile>(&profile, 1));
}

DecodeResult classify_meshtastic(Bytes frame, bool crc, std::span<const Profile> profiles) {
    DecodeResult result;
    if (!crc) { result.status = Status::bad_phy_crc; return result; }
    if (frame.size() <= 16 || frame.size() > max_frame) return result;
    if (profiles.size() > max_keyring_profiles) {
        result.status = Status::invalid_profile; return result;
    }
    // Validate the complete configuration before any attempt can yield classification evidence;
    // a malformed later profile must not turn order into partial authorization.
    for (const auto& profile : profiles) {
        if ((profile.restrict_channel_name && profile.channel_name.empty()) || profile.channel_name.size() > 32 ||
            profile.id.empty() || profile.id.size() > 128 || profile.keys.size() > max_profile_keys) {
            result.status = Status::invalid_profile; return result;
        }
        for (const auto& key : profile.keys) {
            if (key.bytes().size() != 16 && key.bytes().size() != 32) {
                result.status = Status::invalid_profile; return result;
            }
        }
    }
    const auto to = le32(frame), from = le32(frame.subspan(4)), id = le32(frame.subspan(8));
    if (from == 0 || from == 0xffffffffU || to == 0) return result;
    // Channel zero + unicast is also the PKI convention. No recipient-key
    // implementation is installed, so reject this ambiguous mode conservatively.
    if (frame[13] == 0 && to != 0xffffffffU) { result.status = Status::unsupported_pki; return result; }
    result.status = Status::no_matching_key;
    bool matched = false, plausible = false;
    const ChannelKey* successful_key = nullptr;
    for (const auto& profile : profiles) {
        for (const auto& key : profile.keys) {
            if (profile.restrict_channel_name && channel_hash(profile.channel_name, key) != frame[13]) continue;
            matched = true;
            // Same eligible bytes produce the same plaintext for this nonce.
            // Do not invent ambiguity from duplicate user configuration, or a
            // unique channel identity from the weak hash/name association.
            if (successful_key && successful_key->bytes().size() == key.bytes().size() &&
                CRYPTO_memcmp(successful_key->bytes().data(), key.bytes().data(), key.bytes().size()) == 0) {
                continue;
            }
            ClearedBuffer plain;
            if (!crypt(frame.subspan(16), key, meshtastic_nonce(from, id), plain.bytes)) {
                result.evidence.reset(); result.status = Status::crypto_error; return result;
            }
            EnvelopeEvidence candidate;
            const auto parsed = parse_data(Bytes(plain.bytes.data(), frame.size() - 16), candidate);
            if (!parsed) continue;
            if (plausible) {
                result.evidence.reset(); result.status = Status::ambiguous_keys; return result;
            }
            plausible = true;
            successful_key = &key;
            result.evidence = candidate;
        }
    }
    if (result.evidence && recognized_port(result.evidence->port)) { result.status = Status::classified; result.classification = "likely Meshtastic"; }
    else if (plausible) { result.status = Status::unsupported_payload; result.classification = "possible Meshtastic"; }
    else if (matched) result.status = Status::decryption_unvalidated;
    return result;
}

Profile synthetic_profile() {
    Profile profile; profile.id = "synthetic-demo"; profile.label = "Synthetic demo only";
    profile.channel_name = "OVMeshSynthetic";
    // Deterministic artificial fixture; never installed in a live profile.
    auto key = ChannelKey::from_hex("000102030405060708090a0b0c0d0e0f");
    profile.keys.push_back(std::move(*key)); return profile;
}
std::vector<std::uint8_t> synthetic_text_frame(const Profile& profile, std::uint32_t id, std::string_view text) {
    if (profile.keys.size() != 1 || text.empty() || text.size() > 233) return {};
    ClearedBuffer plaintext; std::size_t size{};
    plaintext.bytes[size++] = 8; plaintext.bytes[size++] = 1; plaintext.bytes[size++] = 18;
    if (text.size() > 127) { plaintext.bytes[size++] = static_cast<std::uint8_t>((text.size() & 127) | 128); plaintext.bytes[size++] = 1; }
    else plaintext.bytes[size++] = static_cast<std::uint8_t>(text.size());
    std::copy(text.begin(), text.end(), plaintext.bytes.begin() + static_cast<std::ptrdiff_t>(size)); size += text.size();
    std::vector<std::uint8_t> frame(16 + size, 0); put32(frame.data(), 0xffffffffU);
    put32(frame.data() + 4, 0x10203040U); put32(frame.data() + 8, id);
    frame[12] = 0x63; frame[13] = channel_hash(profile.channel_name, profile.keys.front());
    if (!crypt(Bytes(plaintext.bytes.data(), size), profile.keys.front(), meshtastic_nonce(0x10203040U, id),
               std::span<std::uint8_t>(frame).subspan(16))) return {};
    return frame;
}
} // namespace ovmesh::protocol
