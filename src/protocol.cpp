// SPDX-License-Identifier: GPL-3.0-or-later
// Wire behavior follows pinned Meshtastic firmware/protobuf definitions. See
// docs/research/meshtastic-wire-baseline.md. No device APIs or upstream key policy.
#include "ovmesh/protocol.hpp"
#include "protobuf_guard.hpp"
#include <pb_decode.h>
#include <meshtastic/mesh.pb.h>
#include <meshtastic/telemetry.pb.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/provider.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
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

bool text_value(Bytes data, std::size_t maximum, std::string& out, bool multiline = false) {
    if (data.size() > maximum) return false;
    // Validate Unicode scalar values, overlong encodings, and display controls.
    for (std::size_t i = 0; i < data.size();) {
        const auto b = data[i++];
        if (b < 128) {
            if ((b < 32 && !(multiline && (b == 9 || b == 10 || b == 13))) || b == 127) return false;
            continue;
        }
        unsigned extra; std::uint32_t value, minimum;
        if (b >= 0xc2 && b <= 0xdf) { extra = 1; value = b & 31; minimum = 128; }
        else if (b >= 0xe0 && b <= 0xef) { extra = 2; value = b & 15; minimum = 2048; }
        else if (b >= 0xf0 && b <= 0xf4) { extra = 3; value = b & 7; minimum = 65536; }
        else return false;
        if (data.size() - i < extra) return false;
        for (unsigned j = 0; j < extra; ++j) {
            const auto c = data[i++]; if ((c & 0xc0) != 0x80) return false;
            value = (value << 6) | (c & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
            (value >= 0x80 && value <= 0x9f) || (value >= 0x202a && value <= 0x202e) ||
            (value >= 0x2066 && value <= 0x2069)) return false;
    }
    out.assign(reinterpret_cast<const char*>(data.data()), data.size());
    return true;
}

enum class Parse { invalid, supported, unsupported };

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
template<std::size_t N>
bool projected_string(const char (&input)[N], std::size_t maximum, std::string& output) {
    const auto end = std::find(std::begin(input), std::end(input), '\0');
    if (end == std::end(input)) return false;
    return text_value(Bytes(reinterpret_cast<const std::uint8_t*>(input),
                            static_cast<std::size_t>(end - input)), maximum, output);
}

Parse position(Bytes bytes, DecodedContent& content) {
    ClearedMessage<meshtastic_Position> decoded;
    auto& value = decoded.value;
    if (!decode_message(bytes, &meshtastic_Position_msg, value)) return Parse::invalid;
    if (!value.has_latitude_i || !value.has_longitude_i) return Parse::unsupported;
    const double latitude = value.latitude_i * 1e-7;
    const double longitude = value.longitude_i * 1e-7;
    if (std::abs(latitude) > 90 || std::abs(longitude) > 180) return Parse::invalid;
    content.latitude = latitude; content.longitude = longitude;
    if (value.has_altitude) content.altitude = value.altitude;
    if (detail::has_wire_field(bytes, meshtastic_Position_time_tag)) content.reported_time = value.time;
    content.kind = "position";
    return Parse::supported;
}

Parse node(Bytes bytes, DecodedContent& content) {
    ClearedMessage<meshtastic_User> decoded;
    auto& value = decoded.value;
    if (!decode_message(bytes, &meshtastic_User_msg, value) ||
        !projected_string(value.id, 40, content.node_id) ||
        !projected_string(value.long_name, 40, content.long_name) ||
        !projected_string(value.short_name, 5, content.short_name)) return Parse::invalid;
    if ((value.macaddr.size != 0 && value.macaddr.size != 6) ||
        (value.public_key.size != 0 && value.public_key.size != 32)) return Parse::invalid;
    if (detail::has_wire_field(bytes, meshtastic_User_hw_model_tag))
        content.hardware_model = static_cast<std::uint32_t>(value.hw_model);
    if (detail::has_wire_field(bytes, meshtastic_User_role_tag))
        content.role = static_cast<std::uint32_t>(value.role);
    if (content.node_id.empty() && content.long_name.empty() && content.short_name.empty()) return Parse::unsupported;
    content.kind = "node";
    return Parse::supported;
}

Parse device_metrics(const meshtastic_DeviceMetrics& value, DecodedContent& content) {
    bool projected = false;
    // Values above 100 mean powered; do not turn that into a battery percentage.
    if (value.has_battery_level && value.battery_level <= 100) {
        content.battery_percent = value.battery_level; projected = true;
    }
    if (value.has_voltage) {
        if (!std::isfinite(value.voltage) || value.voltage < 0) return Parse::invalid;
        content.voltage = value.voltage; projected = true;
    }
    if (value.has_channel_utilization) {
        if (!std::isfinite(value.channel_utilization) || value.channel_utilization < 0 || value.channel_utilization > 100) return Parse::invalid;
        content.channel_utilization = value.channel_utilization; projected = true;
    }
    if (value.has_air_util_tx) {
        if (!std::isfinite(value.air_util_tx) || value.air_util_tx < 0 || value.air_util_tx > 100) return Parse::invalid;
        content.air_util_tx = value.air_util_tx; projected = true;
    }
    if (!projected) return Parse::unsupported;
    content.kind = "device telemetry";
    return Parse::supported;
}

Parse environment_metrics(const meshtastic_EnvironmentMetrics& value, DecodedContent& content) {
    bool projected = false;
    if (value.has_temperature) {
        if (!std::isfinite(value.temperature)) return Parse::invalid;
        content.temperature = value.temperature; projected = true;
    }
    if (value.has_relative_humidity) {
        if (!std::isfinite(value.relative_humidity) || value.relative_humidity < 0 || value.relative_humidity > 100) return Parse::invalid;
        content.humidity = value.relative_humidity; projected = true;
    }
    if (value.has_voltage) {
        if (!std::isfinite(value.voltage) || value.voltage < 0) return Parse::invalid;
        content.voltage = value.voltage; projected = true;
    }
    if (!projected) return Parse::unsupported;
    content.kind = "environment telemetry";
    return Parse::supported;
}

Parse telemetry(Bytes bytes, DecodedContent& content) {
    ClearedMessage<meshtastic_Telemetry> decoded;
    auto& value = decoded.value;
    if (!decode_message(bytes, &meshtastic_Telemetry_msg, value)) return Parse::invalid;
    if (detail::has_wire_field(bytes, meshtastic_Telemetry_time_tag)) content.reported_time = value.time;
    if (value.which_variant == meshtastic_Telemetry_device_metrics_tag)
        return device_metrics(value.variant.device_metrics, content);
    if (value.which_variant == meshtastic_Telemetry_environment_metrics_tag)
        return environment_metrics(value.variant.environment_metrics, content);
    return Parse::unsupported;
}

void project_route(const meshtastic_RouteDiscovery& value, DecodedContent& content) {
    content.route.assign(value.route, value.route + value.route_count);
    content.route_back.assign(value.route_back, value.route_back + value.route_back_count);
    content.snr_towards.assign(value.snr_towards, value.snr_towards + value.snr_towards_count);
    content.snr_back.assign(value.snr_back, value.snr_back + value.snr_back_count);
}
Parse route(Bytes bytes, DecodedContent& content) {
    ClearedMessage<meshtastic_RouteDiscovery> decoded;
    if (!decode_message(bytes, &meshtastic_RouteDiscovery_msg, decoded.value)) return Parse::invalid;
    project_route(decoded.value, content);
    // A request with no visited hops is a valid empty protobuf message.
    content.kind = "traceroute";
    return Parse::supported;
}

Parse routing(Bytes bytes, DecodedContent& content) {
    ClearedMessage<meshtastic_Routing> decoded;
    auto& value = decoded.value;
    if (!decode_message(bytes, &meshtastic_Routing_msg, value)) return Parse::invalid;
    switch (value.which_variant) {
    case meshtastic_Routing_route_request_tag:
        project_route(value.variant.route_request, content); content.routing_variant = "request"; break;
    case meshtastic_Routing_route_reply_tag:
        project_route(value.variant.route_reply, content); content.routing_variant = "reply"; break;
    case meshtastic_Routing_error_reason_tag: {
        const auto error = static_cast<std::uint32_t>(value.variant.error_reason);
        if (error > 9 && (error < 32 || error > 39)) return Parse::unsupported;
        content.routing_error = error; content.routing_variant = "error"; break;
    }
    default: return Parse::unsupported; // Absent variant is never an ACK/NONE.
    }
    content.kind = "routing";
    return Parse::supported;
}

Parse payload(std::uint32_t port, Bytes bytes, DecodedContent& content) {
    if (port == meshtastic_PortNum_TEXT_MESSAGE_APP) {
        content.kind = "text";
        return !bytes.empty() && text_value(bytes, 233, content.text, true) ? Parse::supported : Parse::invalid;
    }
    if (port == meshtastic_PortNum_POSITION_APP) return position(bytes, content);
    if (port == meshtastic_PortNum_NODEINFO_APP) return node(bytes, content);
    if (port == meshtastic_PortNum_ROUTING_APP) return routing(bytes, content);
    if (port == meshtastic_PortNum_TELEMETRY_APP) return telemetry(bytes, content);
    if (port == meshtastic_PortNum_TRACEROUTE_APP) return route(bytes, content);
    return Parse::unsupported;
}

Parse parse_data(Bytes bytes, AuthorizedContent& out) {
    ClearedMessage<meshtastic_Data> decoded;
    auto& value = decoded.value;
    if (!decode_message(bytes, &meshtastic_Data_msg, value)) return Parse::invalid;
    const auto port = static_cast<std::uint32_t>(value.portnum);
    // Port zero cannot identify an application. Retain only bounded numeric
    // evidence for unsupported ports, not arbitrary plaintext or identifiers.
    if (port == 0 || port > 65535 || value.payload.size > 233 ||
        (value.xeddsa_signature.size != 0 && value.xeddsa_signature.size != 64)) return Parse::invalid;
    if (!detail::has_wire_field(bytes, meshtastic_Data_payload_tag) && port != meshtastic_PortNum_TRACEROUTE_APP) return Parse::invalid;
    out.port = port; out.want_response = value.want_response;
    out.request_id = value.request_id; out.reply_id = value.reply_id;
    out.signature_present = value.xeddsa_signature.size != 0;
    return payload(port, Bytes(value.payload.bytes, value.payload.size), out.content);
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
        // Explicit public index 1 only; never installed or tried implicitly.
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
    case Status::decoded: return "decoded"; case Status::bad_phy_crc: return "bad phy CRC";
    case Status::invalid_frame: return "invalid frame"; case Status::no_matching_key: return "no matching key";
    case Status::decryption_unvalidated: return "decryption unvalidated";
    case Status::unsupported_payload: return "unsupported payload"; case Status::ambiguous_keys: return "ambiguous keys";
    case Status::unsupported_pki: return "unsupported PKI"; case Status::invalid_profile: return "invalid profile";
    case Status::crypto_error: return "crypto error";
    }
    return "invalid frame";
}

DecodeResult decode_meshtastic(Bytes frame, bool crc, const Profile& profile) {
    return decode_meshtastic(frame, crc, std::span<const Profile>(&profile, 1));
}

DecodeResult decode_meshtastic(Bytes frame, bool crc, std::span<const Profile> profiles) {
    DecodeResult result;
    if (!crc) { result.status = Status::bad_phy_crc; return result; }
    if (frame.size() <= 16 || frame.size() > max_frame) return result;
    if (profiles.size() > max_keyring_profiles) {
        result.status = Status::invalid_profile; return result;
    }
    // Validate the complete configuration before any attempt can yield content;
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
    std::string_view successful_profile_id;
    for (const auto& profile : profiles) {
        for (const auto& key : profile.keys) {
            if (profile.restrict_channel_name && channel_hash(profile.channel_name, key) != frame[13]) continue;
            matched = true;
            // Same eligible bytes produce the same plaintext for this nonce.
            // Do not invent ambiguity from duplicate user configuration, or a
            // unique channel identity from the weak hash/name association.
            if (successful_key && successful_key->bytes().size() == key.bytes().size() &&
                CRYPTO_memcmp(successful_key->bytes().data(), key.bytes().data(), key.bytes().size()) == 0) {
                if (result.authorized && profile.id != successful_profile_id)
                    result.authorized->profile_id = "configured-keyring";
                continue;
            }
            ClearedBuffer plain;
            if (!crypt(frame.subspan(16), key, meshtastic_nonce(from, id), plain.bytes)) {
                result.authorized.reset(); result.evidence.reset(); result.status = Status::crypto_error; return result;
            }
            AuthorizedContent candidate;
            const auto parsed = parse_data(Bytes(plain.bytes.data(), frame.size() - 16), candidate);
            if (parsed == Parse::invalid) continue;
            if (plausible) {
                result.authorized.reset(); result.evidence.reset(); result.status = Status::ambiguous_keys; return result;
            }
            plausible = true;
            successful_key = &key;
            successful_profile_id = profile.id;
            result.evidence = EnvelopeEvidence{candidate.port, candidate.signature_present};
            if (parsed == Parse::unsupported) continue;
            candidate.profile_id = profile.id; candidate.from = from; candidate.to = to; candidate.packet_id = id;
            candidate.hop_limit = frame[12] & 7; candidate.hop_start = (frame[12] >> 5) & 7;
            candidate.want_ack = (frame[12] & 8) != 0; candidate.via_mqtt = (frame[12] & 16) != 0;
            candidate.channel_hash = frame[13]; candidate.next_hop = frame[14]; candidate.relay_node = frame[15];
            result.authorized = std::move(candidate);
        }
    }
    if (result.authorized) { result.status = Status::decoded; result.classification = "likely Meshtastic"; }
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
