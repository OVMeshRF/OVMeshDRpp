// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/protocol.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>

namespace {
using namespace ovmesh::protocol;
using Bytes = std::vector<std::uint8_t>;
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void varint(Bytes& b, std::uint64_t value) {
    do { auto v = static_cast<std::uint8_t>(value & 127); value >>= 7;
         b.push_back(static_cast<std::uint8_t>(v | (value ? 128 : 0))); } while (value);
}
void scalar(Bytes& b, unsigned number, std::uint64_t value) { varint(b, number << 3); varint(b, value); }
void fixed(Bytes& b, unsigned number, std::uint32_t value) {
    varint(b, (number << 3) | 5); for (unsigned i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
void floating(Bytes& b, unsigned number, float value) { fixed(b, number, std::bit_cast<std::uint32_t>(value)); }
void blob(Bytes& b, unsigned number, const Bytes& value) {
    varint(b, (number << 3) | 2); varint(b, value.size()); b.insert(b.end(), value.begin(), value.end());
}
Bytes string_bytes(std::string_view value) { return Bytes(value.begin(), value.end()); }
Bytes envelope(unsigned port, const Bytes& payload) { Bytes b; scalar(b, 1, port); blob(b, 2, payload); return b; }

// Independent mode implementation for tests: encrypt individual nonce/counter
// blocks with AES-ECB and XOR plaintext. The production EVP CTR path is not used.
Bytes frame_for(const Profile& profile, const Bytes& plaintext, std::uint32_t id = 0x78563412,
                std::uint32_t sender = 0x12345678) {
    const auto& key = profile.keys.front();
    Bytes frame(16 + plaintext.size());
    auto set32 = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) frame[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    set32(0, 0xffffffffU); set32(4, sender); set32(8, id); frame[12] = 0x63;
    std::uint8_t hash{}; for (char c : profile.channel_name) hash ^= static_cast<unsigned char>(c);
    for (auto c : key.bytes()) hash ^= c; frame[13] = hash;
    auto* ctx = EVP_CIPHER_CTX_new(); require(ctx, "test cipher allocation");
    require(EVP_EncryptInit_ex(ctx, key.bytes().size() == 16 ? EVP_aes_128_ecb() : EVP_aes_256_ecb(), nullptr, key.bytes().data(), nullptr) == 1, "test cipher init");
    require(EVP_CIPHER_CTX_set_padding(ctx, 0) == 1, "test no padding");
    for (std::size_t offset = 0; offset < plaintext.size(); offset += 16) {
        std::array<std::uint8_t, 16> nonce{}, stream{};
        for (unsigned i = 0; i < 4; ++i) { nonce[i] = static_cast<std::uint8_t>(id >> (8 * i)); nonce[8 + i] = static_cast<std::uint8_t>(sender >> (8 * i)); }
        const auto counter = static_cast<std::uint32_t>(offset / 16);
        for (unsigned i = 0; i < 4; ++i) nonce[15 - i] = static_cast<std::uint8_t>(counter >> (8 * i));
        int count{}; require(EVP_EncryptUpdate(ctx, stream.data(), &count, nonce.data(), 16) == 1 && count == 16, "test ECB block");
        for (std::size_t i = 0; i < 16 && offset + i < plaintext.size(); ++i) frame[16 + offset + i] = plaintext[offset + i] ^ stream[i];
    }
    EVP_CIPHER_CTX_free(ctx); return frame;
}

void explicit_key_input() {
    constexpr std::string_view hex128 = "000102030405060708090a0b0c0d0e0f";
    constexpr std::string_view hex256 = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    constexpr std::string_view base128 = "AAECAwQFBgcICQoLDA0ODw==";
    constexpr std::string_view base256 = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
    auto same = [](std::string_view input, std::string_view expected_hex) {
        auto key = ChannelKey::from_user_input(input), expected = ChannelKey::from_hex(expected_hex);
        require(key && expected && std::ranges::equal(key->bytes(), expected->bytes()), "explicit key encodings match expected bytes");
    };
    same(hex128, hex128); same(hex256, hex256);
    same("000102030405060708090A0B0C0D0E0F", hex128);
    same(base128, hex128); same(base256, hex256);
    same(std::string(21, '/') + "w==", std::string(32, 'f'));
    same(std::string(42, '/') + "8=", std::string(64, 'f'));
    same(std::string(22, 'A') + "==", std::string(32, '0'));
    same(std::string(43, 'A') + "=", std::string(64, '0'));
    // Published Meshtastic Channels.h defaultpsk, explicitly selected only.
    constexpr std::string_view public_hex = "d4f1bb3a20290759f0bcffabcf4e6901";
    same("AQ==", public_hex);
    same("1PG7OiApB1nwvP+rz05pAQ==", public_hex);
    for (std::string_view bad : {
            "", " ", "AA==", "AQ", "AQ=", "AQ===", "AQ==\n", " AQ==", "AQ== ",
            "AR==", "Ag==", "////", "0x000102030405060708090a0b0c0d0e0f",
            "AAECAwQFBgcICQoLDA0ODw", "AAECAwQFBgcICQoLDA0ODw=", "AAECAwQFBgcICQoLDA0ODw===",
            "AAECAwQFBgcICQoLDA0ODwAA", "AAECAwQFBgcICQoLDA0ODw=A", "AAECAwQFBgcICQoLDA0O=w==",
            "1PG7OiApB1nwvP-rz05pAQ==", "_____________________w==",
            "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8", "AAAAAAAAAAAAAAAAAAAAAAAAAAA=",
            "AAECAwQFBgcICQoLDA0ODw==garbage"})
        require(!ChannelKey::from_user_input(bad), "invalid key syntax rejected");
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (unsigned index = 0; index < 256; ++index) {
        std::string input{alphabet[index >> 2], alphabet[(index & 3) << 4], '=', '='};
        require(ChannelKey::from_user_input(input).has_value() == (index == 1), "only explicit public index one is supported");
    }
    for (unsigned unused = 1; unused < 16; ++unused) {
        std::string input(base128); input[21] = alphabet[48 + unused];
        require(!ChannelKey::from_user_input(input), "AES128 noncanonical padding bits rejected");
    }
    for (unsigned unused = 1; unused < 4; ++unused) {
        std::string input(base256); input[42] = alphabet[60 + unused];
        require(!ChannelKey::from_user_input(input), "AES256 noncanonical padding bits rejected");
    }
    for (char invalid : {' ', '\t', '\r', '\n', '\0', static_cast<char>(0x80), '-', '_'}) {
        std::string input(base256); input[20] = invalid;
        require(!ChannelKey::from_user_input(input), "embedded whitespace, NUL and nonstandard alphabet rejected");
    }
    auto parsed = ChannelKey::from_user_input(base256);
    require(parsed.has_value(), "base64 move fixture parsed");
    ChannelKey moved = std::move(*parsed);
    require(parsed->bytes().empty() && moved.bytes().size() == 32, "base64 key move clears source");
    auto profile = synthetic_profile();
    const auto synthetic = frame_for(profile, envelope(1, string_bytes("Explicit Base64 fixture")));
    profile.keys.clear(); profile.keys.push_back(std::move(*ChannelKey::from_user_input(base128)));
    require(decode_meshtastic(synthetic, true, profile).authorized.has_value(), "explicit base64 key decrypts independently generated fixture");
    profile.keys.clear(); profile.keys.push_back(std::move(*ChannelKey::from_hex(public_hex)));
    const auto public_frame = frame_for(profile, envelope(1, string_bytes("Explicit public shorthand fixture")));
    profile.keys.clear();
    require(decode_meshtastic(public_frame, true, profile).status == Status::no_matching_key, "known public key is never an implicit fallback");
    profile.keys.push_back(std::move(*ChannelKey::from_user_input("AQ==")));
    require(decode_meshtastic(public_frame, true, profile).authorized.has_value(), "explicit public shorthand decrypts expected fixture");
}

void protocol_vectors() {
    auto profile = synthetic_profile();
    const std::array<std::uint8_t, 16> expected{0x12,0x34,0x56,0x78,0,0,0,0,0x78,0x56,0x34,0x12,0,0,0,0};
    require(meshtastic_nonce(0x12345678, 0x78563412) == expected, "firmware nonce byte order");
    auto text = string_bytes("Independent multi-block protocol fixture with nonzero sender and packet ID");
    auto frame = frame_for(profile, envelope(1, text));
    auto decoded = decode_meshtastic(frame, true, profile);
    require(decoded.status == Status::decoded && decoded.authorized, "independent AES-ECB counter fixture");
    require(decoded.authorized->content.text == std::string(text.begin(), text.end()), "text projection");
    require(decoded.authorized->from == 0x12345678 && decoded.authorized->packet_id == 0x78563412, "RF header projection");
    require(decoded.classification == "likely Meshtastic" && decoded.authentication == "not authenticated", "CTR does not authenticate identity");
    require(decoded.authorized->hop_limit == 3 && decoded.authorized->hop_start == 3, "hop bitfields");
    require(!decode_meshtastic(frame, false, profile).authorized, "bad PHY CRC prevents plaintext");
    auto copy = frame; copy[16] ^= 0xff;
    require(!decode_meshtastic(copy, true, profile).authorized, "corrupted port rejected");
    // AES-CTR is malleable: some alterations still form valid text. This test
    // makes the limitation explicit rather than pretending CRC/schema is a MAC.
    copy = frame; copy[20] ^= 1;
    auto modified = decode_meshtastic(copy, true, profile);
    require(modified.authorized && modified.authentication == "not authenticated", "malleability truthfulness");
    auto key256 = ChannelKey::from_hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    profile.keys.clear(); profile.keys.push_back(std::move(*key256));
    decoded = decode_meshtastic(frame_for(profile, envelope(1, text)), true, profile);
    require(decoded.authorized && decoded.authorized->content.text == std::string(text.begin(), text.end()), "AES-256 independent fixture");
}

void typed_schemas() {
    auto profile = synthetic_profile();
    Bytes position; fixed(position,1,static_cast<std::uint32_t>(-450000000)); fixed(position,2,900000000);
    scalar(position,3,static_cast<std::uint64_t>(std::int64_t{-10})); fixed(position,4,1700000000);
    auto r = decode_meshtastic(frame_for(profile,envelope(3,position)),true,profile);
    require(r.authorized && r.authorized->content.latitude == -45.0 && r.authorized->content.longitude == 90.0, "Position sfixed32 is not zigzag");
    require(r.authorized->content.altitude == -10.0 && r.authorized->content.reported_time == 1700000000, "Position signed altitude and fixed32 time");
    Bytes old_position; scalar(old_position,1,900000000); scalar(old_position,2,1800000000);
    require(!decode_meshtastic(frame_for(profile,envelope(3,old_position)),true,profile).authorized, "unsupported old positional wire interpretations rejected");
    Bytes user; blob(user,1,string_bytes("!12345678")); blob(user,2,string_bytes("Synthetic node")); blob(user,3,string_bytes("TEST")); scalar(user,5,43); scalar(user,7,2);
    r=decode_meshtastic(frame_for(profile,envelope(4,user)),true,profile);
    require(r.authorized && r.authorized->content.kind == "node" && r.authorized->content.role == 2, "User role is field 7");
    Bytes metrics; scalar(metrics,1,80); floating(metrics,2,3.7F); floating(metrics,3,2.5F); floating(metrics,4,1.25F);
    Bytes telemetry; fixed(telemetry,1,1700000001); blob(telemetry,2,metrics);
    r=decode_meshtastic(frame_for(profile,envelope(67,telemetry)),true,profile);
    require(r.authorized && r.authorized->content.battery_percent == 80.0 && std::abs(*r.authorized->content.voltage-3.7)<0.001, "DeviceMetrics uint32 battery and float voltage");
    Bytes environment; floating(environment,1,21.5F); floating(environment,2,44.0F); floating(environment,5,3.2F);
    telemetry.clear(); blob(telemetry,3,environment);
    r=decode_meshtastic(frame_for(profile,envelope(67,telemetry)),true,profile);
    require(r.authorized && r.authorized->content.kind == "environment telemetry" && r.authorized->content.temperature == 21.5, "Environment telemetry");
    blob(telemetry,2,metrics);
    require(!decode_meshtastic(frame_for(profile,envelope(67,telemetry)),true,profile).authorized, "Telemetry oneof ambiguity rejected");
    Bytes route; fixed(route,1,0x11223344); fixed(route,1,0x55667788);
    r=decode_meshtastic(frame_for(profile,envelope(70,route)),true,profile);
    require(r.authorized && r.authorized->content.route.size()==2,"repeated fixed32 route");
    Bytes routing; scalar(routing,3,0);
    r=decode_meshtastic(frame_for(profile,envelope(5,routing)),true,profile);
    require(r.authorized && r.authorized->content.routing_error == 0,"Routing explicit NONE variant");
}

void rejection_and_bounds() {
    auto profile = synthetic_profile(); const auto frame=synthetic_text_frame(profile);
    require(!ChannelKey::from_hex("") && !ChannelKey::from_hex("AQ==") && !ChannelKey::from_hex(std::string(32,'z')), "only complete hex keys");
    auto moved = ChannelKey::from_hex("000102030405060708090a0b0c0d0e0f"); ChannelKey key=std::move(*moved);
    require(moved->bytes().empty() && key.bytes().size()==16,"move-only source cleared");
    auto no_keys=synthetic_profile(); no_keys.keys.clear();
    require(decode_meshtastic(frame,true,no_keys).status==Status::no_matching_key,"no implicit keys");
    auto wrong=synthetic_profile(); wrong.keys.clear();
    auto wrong_key=ChannelKey::from_hex("101112131415161718191a1b1c1d1e1f"); wrong.keys.push_back(std::move(*wrong_key));
    require(channel_hash(wrong.channel_name,wrong.keys.front())==frame[13],"fixture deliberately collides in 8-bit channel hash");
    require(!decode_meshtastic(frame,true,wrong).authorized,"wrong matching-hash key rejected");
    auto duplicate=ChannelKey::from_hex("000102030405060708090a0b0c0d0e0f"); profile.keys.push_back(std::move(*duplicate));
    require(decode_meshtastic(frame,true,profile).status==Status::decoded,"duplicate identical configured keys are one candidate");
    profile=synthetic_profile();
    auto abandoned=ChannelKey::from_hex("000102030405060708090a0b0c0d0e0f");
    auto retained=std::move(*abandoned);
    profile.keys.push_back(std::move(*abandoned));
    const auto invalid_profile=decode_meshtastic(frame,true,profile);
    require(invalid_profile.status==Status::invalid_profile && !invalid_profile.authorized,"invalid key after valid key cannot leak partial success");
    profile=synthetic_profile();
    for(std::size_t size=0;size<=16;++size) require(!decode_meshtastic(std::span(frame).first(size),true,profile).authorized,"truncated frame");
    require(!decode_meshtastic(Bytes(256,0),true,profile).authorized,"oversized frame");
    for(const Bytes& plain:std::vector<Bytes>{
        {8,1,8,1,18,1,'a'}, {8,0x81,0,18,1,'a'}, {8,1,18,127,'a'}, {8,1,18,2,0xc0,0x80},
        {8,1,18,1,0}, {8,1,18,1,'a',0}, {8,1,18,1,'a',0x1b}, {8,1,18,1,'a',0x28,0xff},
        {8,1,18,1,'a',0x18,2}, {8,1,18,1,'a',0x80,0x80,0x80,0x80,0x10,0}})
        require(!decode_meshtastic(frame_for(profile,plain),true,profile).authorized,"malformed protobuf/Unicode has no retained content");
    auto unknown=decode_meshtastic(frame_for(profile,envelope(256,{1,2,3,4})),true,profile);
    require(!unknown.authorized && unknown.status==Status::unsupported_payload,"unknown port no raw fallback");
    auto pki=frame; pki[0]=1;pki[1]=pki[2]=pki[3]=0;pki[13]=0;
    require(decode_meshtastic(pki,true,profile).status==Status::unsupported_pki,"PKI convention cleanly excluded");
    std::mt19937 random(625);
    for(unsigned i=0;i<20000;++i) {
        Bytes b(random()%280);for(auto& byte:b)byte=static_cast<std::uint8_t>(random());
        auto result=decode_meshtastic(b,true,profile);
        require(!result.authorized,"random input not retained");
    }
}
void frequency_independent_keyring() {
    auto source = synthetic_profile();
    const auto frame = frame_for(source, envelope(1, string_bytes("Explicit keyring fixture")));
    auto require_empty = [](const DecodeResult& result, Status status, const char* description) {
        require(result.status == status && !result.authorized && !result.evidence &&
                result.classification == "unknown LoRa" && result.authentication == "not authenticated", description);
    };
    require_empty(decode_meshtastic(frame, true, std::span<const Profile>{}),
                  Status::no_matching_key, "empty keyring never installs a fallback");

    std::vector<Profile> profiles;
    auto unrelated = synthetic_profile(); unrelated.id = "unrelated-channel";
    unrelated.channel_name += "B";
    require(channel_hash(unrelated.channel_name, unrelated.keys.front()) != frame[13], "unrelated channel fixture hash");
    profiles.push_back(std::move(unrelated));
    auto wrong = synthetic_profile(); wrong.id = "wrong-colliding-key"; wrong.keys.clear();
    wrong.keys.push_back(std::move(*ChannelKey::from_hex("101112131415161718191a1b1c1d1e1f")));
    require(channel_hash(wrong.channel_name, wrong.keys.front()) == frame[13], "keyring fixture intentionally collides in channel hash");
    profiles.push_back(std::move(wrong));
    require_empty(decode_meshtastic(frame, true, profiles), Status::decryption_unvalidated,
                  "matching hash without valid decryption does not identify mesh traffic");
    auto selected = synthetic_profile(); selected.id = "authorized-channel";
    profiles.push_back(std::move(selected));
    auto result = decode_meshtastic(frame, true, profiles);
    require(result.status == Status::decoded && result.authorized && result.evidence &&
            result.authorized->profile_id == "authorized-channel" &&
            result.authorized->content.text == "Explicit keyring fixture" &&
            result.classification == "likely Meshtastic" && result.authentication == "not authenticated",
            "keyring selects explicit channel independently of any receiver lane");
    std::reverse(profiles.begin(), profiles.end());
    result = decode_meshtastic(frame, true, profiles);
    require(result.authorized && result.authorized->profile_id == "authorized-channel", "unique successful profile independent of ordering");

    const auto unsupported_frame = frame_for(source, envelope(256, {1,2,3,4}));
    result = decode_meshtastic(unsupported_frame, true, profiles);
    require(result.status == Status::unsupported_payload && !result.authorized && result.evidence &&
            result.evidence->port == 256 && result.classification == "possible Meshtastic",
            "unique keyring unsupported envelope retains only bounded evidence");

    auto alias = synthetic_profile(); alias.id = "same-key-different-channel"; alias.channel_name += "AA";
    require(alias.channel_name != source.channel_name && channel_hash(alias.channel_name, alias.keys.front()) == frame[13],
            "distinct channel names can share the same key and weak hash");
    profiles.push_back(std::move(alias));
    result = decode_meshtastic(frame, true, profiles);
    require(result.authorized && result.authorized->profile_id == "configured-keyring" && result.evidence &&
            result.authorized->content.text == "Explicit keyring fixture",
            "same-key aliases decode without inventing unique channel provenance");
    result = decode_meshtastic(unsupported_frame, true, profiles);
    require(result.status == Status::unsupported_payload && !result.authorized && result.evidence && result.evidence->port == 256,
            "duplicate unsupported envelopes preserve bounded evidence");
    std::reverse(profiles.begin(), profiles.end());
    result = decode_meshtastic(frame, true, profiles);
    require(result.authorized && result.authorized->profile_id == "configured-keyring",
            "duplicate alias provenance is order independent");
    require_empty(decode_meshtastic(frame, false, profiles), Status::bad_phy_crc,
                  "bad PHY CRC suppresses every keyring attempt");

    profiles.clear(); profiles.push_back(synthetic_profile());
    auto invalid = synthetic_profile(); invalid.id = "invalid-later-profile";
    auto moved = std::move(invalid.keys.front());
    profiles.push_back(std::move(invalid));
    require_empty(decode_meshtastic(frame, true, profiles), Status::invalid_profile,
                  "invalid later key record prevents partial keyring success");
    profiles.back() = synthetic_profile(); profiles.back().channel_name.clear();
    require_empty(decode_meshtastic(frame, true, profiles), Status::invalid_profile,
                  "invalid channel name prevents every attempt");
    profiles.clear();
    for (std::size_t index = 0; index < max_keyring_profiles; ++index) {
        auto profile = synthetic_profile(); profile.id = "configured-" + std::to_string(index);
        profile.keys.clear(); profiles.push_back(std::move(profile));
    }
    require_empty(decode_meshtastic(frame, true, profiles), Status::no_matching_key,
                  "maximum valid keyless profiles remain bounded");
    profiles.push_back(synthetic_profile());
    require_empty(decode_meshtastic(frame, true, profiles), Status::invalid_profile,
                  "excessive profile count rejected before successful key");
    profiles.clear(); profiles.push_back(synthetic_profile());
    for (std::size_t index = 0; index < max_profile_keys; ++index)
        profiles.front().keys.push_back(std::move(*ChannelKey::from_hex("000102030405060708090a0b0c0d0e0f")));
    require_empty(decode_meshtastic(frame, true, profiles), Status::invalid_profile,
                  "excessive keys in a profile rejected before successful key");

    const auto single = decode_meshtastic(frame, true, source);
    const auto span = decode_meshtastic(frame, true, std::span<const Profile>(&source, 1));
    require(single.status == span.status && single.classification == span.classification &&
            single.authorized && span.authorized && single.authorized->profile_id == span.authorized->profile_id &&
            single.authorized->content.text == span.authorized->content.text,
            "single-profile wrapper preserves supported behavior");

    // Knowing an authorized key does not imply knowing a channel name, preset,
    // hash or frequency. This scope must be selected explicitly by the caller.
    profiles.clear();
    auto key_only = synthetic_profile(); key_only.id = "explicit-survey-key";
    key_only.channel_name.clear(); key_only.restrict_channel_name = false;
    profiles.push_back(std::move(key_only));
    auto other_channel = synthetic_profile(); other_channel.channel_name += "B";
    const auto other_frame = frame_for(other_channel, envelope(1, string_bytes("Other channel, same explicit key")));
    require(frame[13] != other_frame[13], "key-only fixture uses different on-air channel hashes");
    for (const auto& candidate_frame : {frame, other_frame}) {
        result = decode_meshtastic(candidate_frame, true, profiles);
        require(result.authorized && result.authorized->profile_id == "explicit-survey-key" &&
                result.authorized->channel_hash == candidate_frame[13] && result.authentication == "not authenticated",
                "explicit key-only profile decodes independently of channel name and hash");
    }
    const auto unrelated_key_frame = [&] {
        auto profile = synthetic_profile(); profile.keys.clear();
        profile.keys.push_back(std::move(*ChannelKey::from_hex("101112131415161718191a1b1c1d1e1f")));
        return frame_for(profile, envelope(1, string_bytes("Other key fixture")));
    }();
    require_empty(decode_meshtastic(unrelated_key_frame, true, profiles), Status::decryption_unvalidated,
                  "key-only discovery never adds an unrelated key or retains wrong-key bytes");
    require_empty(decode_meshtastic(frame, false, profiles), Status::bad_phy_crc,
                  "key-only profile still requires valid PHY CRC");
    result = decode_meshtastic(unsupported_frame, true, profiles);
    require(result.status == Status::unsupported_payload && !result.authorized && result.evidence && result.evidence->port == 256,
            "key-only unsupported envelopes expose bounded evidence only");
    auto restricted_alias = synthetic_profile(); restricted_alias.id = "named-alias";
    profiles.push_back(std::move(restricted_alias));
    result = decode_meshtastic(frame, true, profiles);
    require(result.authorized && result.authorized->profile_id == "configured-keyring" && result.evidence,
            "key-only and named aliases decode with nonunique profile provenance");
    result = decode_meshtastic(other_frame, true, profiles);
    require(result.authorized && result.authorized->profile_id == "explicit-survey-key",
            "ineligible named alias does not affect unique key-only provenance");
    profiles.pop_back(); profiles.front().restrict_channel_name = true;
    require_empty(decode_meshtastic(frame, true, profiles), Status::invalid_profile,
                  "empty name needs an explicit key-only choice");
    profiles.front().restrict_channel_name = false; profiles.front().keys.clear();
    require_empty(decode_meshtastic(frame, true, profiles), Status::no_matching_key,
                  "key-only mode never installs default or public keys");

    // Independently derived AES-ECB counter-block fixture: these distinct keys
    // share only the first two keystream bytes for frame_for's default nonce.
    // Both therefore plausibly decode the two-byte empty-traceroute Data 0846.
    // This is a synthetic demonstration of CTR's lack of authentication, not a
    // round-trip-only ambiguity test or a claim that either key is authentic.
    profiles.clear();
    auto first = synthetic_profile(); first.id = "distinct-a"; first.restrict_channel_name = false;
    auto second = synthetic_profile(); second.id = "distinct-b"; second.restrict_channel_name = false;
    second.keys.clear(); second.keys.push_back(std::move(*ChannelKey::from_hex("000102030405060708090a0bb5200000")));
    const Bytes short_data{8,70};
    const auto short_frame = frame_for(first, short_data);
    const auto second_frame = frame_for(second, short_data);
    require(std::equal(short_frame.begin()+16, short_frame.end(), second_frame.begin()+16),
            "independent distinct-key fixture shares two-byte CTR ciphertext");
    require(decode_meshtastic(short_frame, true, first).authorized.has_value() &&
            decode_meshtastic(short_frame, true, second).authorized.has_value(),
            "distinct fixture keys each produce plausible structure alone");
    profiles.push_back(std::move(first)); profiles.push_back(std::move(second));
    require_empty(decode_meshtastic(short_frame, true, profiles), Status::ambiguous_keys,
                  "different plausible key material still suppresses content and evidence");
    std::reverse(profiles.begin(), profiles.end());
    require_empty(decode_meshtastic(short_frame, true, profiles), Status::ambiguous_keys,
                  "distinct-key ambiguity remains order independent");
}
void traceroute_and_evidence() {
    auto profile = synthetic_profile();
    auto decode = [&](const Bytes& data) { return decode_meshtastic(frame_for(profile, data), true, profile); };
    Bytes request; scalar(request, 1, 70); scalar(request, 3, 1);
    auto r = decode(request);
    require(r.authorized && r.authorized->content.kind == "traceroute" && r.authorized->content.route.empty() && r.authorized->want_response,
            "official empty traceroute request may omit payload");
    require(r.evidence && r.evidence->port == 70 && !r.evidence->signature_present, "traceroute evidence separate from content");
    r = decode(envelope(70, {}));
    require(r.authorized && r.authorized->content.route.empty(), "explicit empty traceroute payload");

    Bytes route, packed;
    fixed(route, 1, 0x12345678); fixed(route, 1, 0xffffffffU);
    varint(packed, static_cast<std::uint64_t>(std::int64_t{-31})); varint(packed, 24); blob(route, 2, packed);
    Bytes back{0x44,0x33,0x22,0x11}; blob(route, 3, back);
    scalar(route, 4, static_cast<std::uint64_t>(std::int64_t{-128}));
    auto data = envelope(70, route); fixed(data, 6, 0x78563412); fixed(data, 7, 0x10203040);
    r = decode(data);
    require(r.authorized && r.authorized->request_id == 0x78563412 && r.authorized->reply_id == 0x10203040,
            "request and reply IDs preserved");
    const auto& content = r.authorized->content;
    require(content.route == std::vector<std::uint32_t>({0x12345678,0xffffffffU}) && content.route_back == std::vector<std::uint32_t>({0x11223344}), "both independently sized routes preserved");
    require(content.snr_towards == std::vector<std::int32_t>({-31,24}) && content.snr_back == std::vector<std::int32_t>({-128}), "signed SNR quarter dB and unknown sentinel preserved");
    Bytes extremes; scalar(extremes, 2, static_cast<std::uint64_t>(std::int64_t{-2147483648LL})); scalar(extremes, 4, 2147483647);
    r = decode(envelope(70, extremes));
    require(r.authorized && r.authorized->content.snr_towards[0] == INT32_MIN && r.authorized->content.snr_back[0] == INT32_MAX, "wire SNR is full int32, not firmware int8 storage");
    Bytes many;
    for (unsigned n = 0; n < 20; ++n) { fixed(many, 1, n + 1); fixed(many, 3, n + 101); }
    r = decode(envelope(70, many));
    require(r.authorized && r.authorized->content.route.size() == 20 && r.authorized->content.route_back.size() == 20, "route bounds independent, no combined 32 limit");
    many.clear(); for (unsigned n = 0; n < 33; ++n) fixed(many, 1, n + 1);
    require(!decode(envelope(70, many)).authorized, "oversized route rejected");
    Bytes overflow; scalar(overflow, 2, 0x80000000ULL);
    require(!decode(envelope(70, overflow)).authorized, "non sign extended out of range int32 rejected");

    r = decode(envelope(5, {}));
    require(!r.authorized && r.evidence && r.status == Status::unsupported_payload, "absent Routing variant is not ACK");
    for (unsigned variant = 1; variant <= 2; ++variant) {
        Bytes routing; blob(routing, variant, {}); r = decode(envelope(5, routing));
        require(r.authorized && r.authorized->content.routing_variant == (variant == 1 ? "request" : "reply") && r.authorized->content.route.empty(), "empty Routing oneof preserved");
    }
    Bytes ack; scalar(ack, 3, 0); r = decode(envelope(5, ack));
    require(r.authorized && r.authorized->content.routing_error == 0 && r.authorized->content.routing_variant == "error", "explicit NONE remains distinct from absent");

    data = envelope(1, string_bytes("Synthetic signed-shape text")); blob(data, 10, Bytes(64, 0x5a));
    r = decode(data);
    require(r.authorized && r.authorized->signature_present && r.evidence->signature_present && r.authentication == "not authenticated", "2.8 signature presence never claims verification");
    for (unsigned size : {0u, 63u, 65u}) {
        data = envelope(1, string_bytes("Synthetic signature bound")); blob(data, 10, Bytes(size, 0x5a)); r = decode(data);
        require(size == 0 ? (r.authorized && !r.authorized->signature_present) : (!r.authorized && !r.evidence), "signature size policy");
    }
    data = envelope(1, string_bytes("Known text with future envelope field")); scalar(data, 123, 42);
    require(decode(data).authorized.has_value(), "unknown envelope field safely skipped");
    route.clear(); fixed(route, 1, 123); scalar(route, 123, 42);
    require(decode(envelope(70, route)).authorized.has_value(), "unknown route field safely skipped");
    data = envelope(256, {1,2,3,4}); r = decode(data);
    require(!r.authorized && r.evidence && r.evidence->port == 256 && r.classification == "possible Meshtastic" && r.authentication == "not authenticated", "unsupported valid envelope yields bounded evidence only");
    auto duplicate = ChannelKey::from_hex("000102030405060708090a0b0c0d0e0f"); profile.keys.push_back(std::move(*duplicate));
    r = decode(data);
    require(r.status == Status::unsupported_payload && !r.authorized && r.evidence && r.evidence->port == 256,
            "identical unsupported key entries share one bounded envelope candidate");
    profile = synthetic_profile();
    Bytes malformed{0x08,0x01}; r = decode(envelope(70, malformed));
    require(!r.authorized && !r.evidence && r.status == Status::decryption_unvalidated, "malformed known payload does not become protocol evidence");
    auto frame = frame_for(profile, envelope(70, {}));
    require(!decode_meshtastic(frame, false, profile).evidence, "CRC failure yields no protocol evidence");
    profile.keys.clear();
    require(!decode_meshtastic(frame, true, profile).evidence, "no configured key yields no protocol evidence");
}
void independent_official_fixtures() {
    struct Fixture { std::string_view name, hex, kind; std::uint32_t port, request_id; bool signature; };
    constexpr Fixture fixtures[]{
#include "fixtures/meshtastic-2.7.19.inc"
#include "fixtures/meshtastic-2.8.0.inc"
    };
    auto profile = synthetic_profile();
    for (const auto& fixture : fixtures) {
        auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        Bytes plain;
        for (std::size_t i = 0; i < fixture.hex.size(); i += 2)
            plain.push_back(static_cast<std::uint8_t>((digit(fixture.hex[i]) << 4) | digit(fixture.hex[i+1])));
        auto r = decode_meshtastic(frame_for(profile, plain), true, profile);
        if (!r.evidence || r.evidence->port != fixture.port || r.evidence->signature_present != fixture.signature)
            throw std::runtime_error("Official fixture evidence failed: " + std::string(fixture.name));
        if (fixture.kind.empty()) {
            require(!r.authorized && r.status == Status::unsupported_payload && r.classification == "possible Meshtastic", "official unsupported app has evidence only");
            continue;
        }
        if (!r.authorized || r.authorized->content.kind != fixture.kind || r.authorized->request_id != fixture.request_id || r.authorized->signature_present != fixture.signature)
            throw std::runtime_error("Official fixture projection failed: " + std::string(fixture.name));
        const auto& c = r.authorized->content;
        require(r.authentication == "not authenticated", "official serializer cannot authenticate sender");
        if (fixture.name.ends_with(":traceroute-reply"))
            require(c.route == std::vector<std::uint32_t>({0x11223344,0xffffffffU}) && c.route_back == std::vector<std::uint32_t>({0x55667788}) && c.snr_towards == std::vector<std::int32_t>({-31,24}) && c.snr_back == std::vector<std::int32_t>({-128}), "official packed arrays and signed SNR");
        if (fixture.name.ends_with(":empty-traceroute")) require(c.route.empty() && r.authorized->want_response, "official empty initiating request");
        if (fixture.name.ends_with(":routing-none")) require(c.routing_error == 0 && c.routing_variant == "error", "official explicit NONE oneof");
        if (fixture.name.ends_with(":position")) require(c.latitude == -45.0 && c.longitude == 90.0 && c.altitude == -10.0, "official position values");
        if (fixture.name.ends_with(":node")) require(c.node_id == "!11223344" && c.short_name == "TEST", "official User strings");
    }
}
}
int main() {
    try { explicit_key_input(); protocol_vectors(); typed_schemas(); traceroute_and_evidence(); independent_official_fixtures(); rejection_and_bounds(); frequency_independent_keyring(); std::cout<<"protocol: explicit key input, independent crypto, official 2.7.19/2.8.0 fixtures, traceroute, evidence, frequency-independent keyring, authorization and bounds checks passed\n"; return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
