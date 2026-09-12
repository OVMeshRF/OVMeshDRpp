#include "ovmesh/meshtastic_presets.hpp"
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
template<class T> concept CarriesSemanticContent = requires(T r) { r.authorized; } || requires(T r) { r.content; };
static_assert(!CarriesSemanticContent<DecodeResult> && !CarriesSemanticContent<EnvelopeEvidence>);
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
void varint(Bytes& b, std::uint64_t value) {
    do { auto v = static_cast<std::uint8_t>(value & 127); value >>= 7;
         b.push_back(static_cast<std::uint8_t>(v | (value ? 128 : 0))); } while (value);
}
void scalar(Bytes& b, unsigned number, std::uint64_t value) { varint(b, number << 3); varint(b, value); }
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
    require(classify_meshtastic(synthetic, true, profile).evidence.has_value(), "explicit base64 key decrypts independently generated fixture");
    profile.keys.clear(); profile.keys.push_back(std::move(*ChannelKey::from_hex(public_hex)));
    const auto public_frame = frame_for(profile, envelope(1, string_bytes("Explicit public shorthand fixture")));
    profile.keys.clear();
    require(classify_meshtastic(public_frame, true, profile).status == Status::no_matching_key, "known public key is never an implicit fallback");
    profile.keys.push_back(std::move(*ChannelKey::from_user_input("AQ==")));
    require(classify_meshtastic(public_frame, true, profile).evidence.has_value(), "explicit public shorthand decrypts expected fixture");
}

void protocol_vectors() {
    auto profile = synthetic_profile();
    const std::array<std::uint8_t, 16> expected{0x12,0x34,0x56,0x78,0,0,0,0,0x78,0x56,0x34,0x12,0,0,0,0};
    require(meshtastic_nonce(0x12345678, 0x78563412) == expected, "firmware nonce byte order");
    auto text = string_bytes("Independent multi-block protocol fixture with nonzero sender and packet ID");
    auto frame = frame_for(profile, envelope(1, text));
    auto result = classify_meshtastic(frame, true, profile);
    require(result.status == Status::classified && result.evidence && result.evidence->port == 1,
            "independent AES-ECB counter fixture yields envelope evidence only");
    require(result.classification == "likely Meshtastic" && result.authentication == "not authenticated",
            "CTR and envelope structure do not authenticate identity or payload");
    require(!classify_meshtastic(frame, false, profile).evidence, "bad PHY CRC prevents classification");
    auto copy = frame; copy[16] ^= 0xff;
    require(!classify_meshtastic(copy, true, profile).evidence, "corrupted envelope rejected");
    copy = frame; copy[20] ^= 1;
    auto modified = classify_meshtastic(copy, true, profile);
    require(modified.evidence && modified.authentication == "not authenticated", "malleability is not hidden");
    profile.keys.clear(); profile.keys.push_back(std::move(*ChannelKey::from_hex(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")));
    result = classify_meshtastic(frame_for(profile, envelope(1, text)), true, profile);
    require(result.evidence && result.evidence->port == 1, "AES-256 independent fixture");
}

void opaque_payloads() {
    auto profile = synthetic_profile();
    // Even text controls, malformed nested protobuf, node/route-like bytes and
    // arbitrary telemetry are never interpreted. Only the outer envelope is checked.
    for (unsigned port : {1u,3u,4u,5u,67u,70u}) {
        for (const auto& payload : std::vector<Bytes>{{}, {0,0xff,0xc0,0x80,0x1b},
                 string_bytes("SYNTHETIC PRIVATE CONTENT MUST NOT LEAVE CLASSIFIER"), Bytes(233,0xaa)}) {
            auto result = classify_meshtastic(frame_for(profile,envelope(port,payload)),true,profile);
            require(result.status == Status::classified && result.evidence && result.evidence->port == port &&
                    result.classification == "likely Meshtastic" && result.authentication == "not authenticated",
                    "opaque application bytes do not become semantic contents or authenticated identity");
        }
    }
    // The returned API cannot carry payload or sender/correlation identities.

}
void rejection_and_bounds() {
    auto profile = synthetic_profile(); const auto frame=synthetic_text_frame(profile);
    require(!ChannelKey::from_hex("") && !ChannelKey::from_hex("AQ==") && !ChannelKey::from_hex(std::string(32,'z')), "only complete hex keys");
    auto moved = ChannelKey::from_hex("000102030405060708090a0b0c0d0e0f"); ChannelKey key=std::move(*moved);
    require(moved->bytes().empty() && key.bytes().size()==16,"move-only source cleared");
    auto no_keys=synthetic_profile(); no_keys.keys.clear();
    require(classify_meshtastic(frame,true,no_keys).status==Status::no_matching_key,"no implicit keys");
    auto wrong=synthetic_profile(); wrong.keys.clear();
    auto wrong_key=ChannelKey::from_hex("101112131415161718191a1b1c1d1e1f"); wrong.keys.push_back(std::move(*wrong_key));
    require(channel_hash(wrong.channel_name,wrong.keys.front())==frame[13],"fixture deliberately collides in 8-bit channel hash");
    require(!classify_meshtastic(frame,true,wrong).evidence,"wrong matching-hash key rejected");
    auto duplicate=ChannelKey::from_hex("000102030405060708090a0b0c0d0e0f"); profile.keys.push_back(std::move(*duplicate));
    require(classify_meshtastic(frame,true,profile).status==Status::classified,"duplicate identical configured keys are one candidate");
    profile=synthetic_profile();
    auto abandoned=ChannelKey::from_hex("000102030405060708090a0b0c0d0e0f");
    auto retained=std::move(*abandoned);
    profile.keys.push_back(std::move(*abandoned));
    const auto invalid_profile=classify_meshtastic(frame,true,profile);
    require(invalid_profile.status==Status::invalid_profile && !invalid_profile.evidence,"invalid key after valid key cannot leak partial success");
    profile=synthetic_profile();
    for(std::size_t size=0;size<=16;++size) require(!classify_meshtastic(std::span(frame).first(size),true,profile).evidence,"truncated frame");
    require(!classify_meshtastic(Bytes(256,0),true,profile).evidence,"oversized frame");
    for(const Bytes& plain:std::vector<Bytes>{
        {8,1,8,1,18,1,'a'}, {8,0x81,0,18,1,'a'}, {8,1,18,127,'a'},
        {8,1,18,1,'a',0}, {8,1,18,1,'a',0x1b}, {8,1,18,1,'a',0x28,0xff},
        {8,1,18,1,'a',0x18,2}, {8,1,18,1,'a',0x80,0x80,0x80,0x80,0x10,0}})
        require(!classify_meshtastic(frame_for(profile,plain),true,profile).evidence,"malformed outer protobuf yields no evidence");
    auto unknown=classify_meshtastic(frame_for(profile,envelope(256,{1,2,3,4})),true,profile);
    require(unknown.evidence && unknown.status==Status::unsupported_payload,"unknown port no raw fallback");
    auto pki=frame; pki[0]=1;pki[1]=pki[2]=pki[3]=0;pki[13]=0;
    require(classify_meshtastic(pki,true,profile).status==Status::unsupported_pki,"PKI convention cleanly excluded");
    std::mt19937 random(625);
    for(unsigned i=0;i<20000;++i) {
        Bytes b(random()%280);for(auto& byte:b)byte=static_cast<std::uint8_t>(random());
        auto result=classify_meshtastic(b,true,profile);
        require(!result.evidence,"random input not retained");
    }
}
void frequency_independent_keyring() {
    auto source = synthetic_profile();
    const auto frame = frame_for(source, envelope(1, string_bytes("Explicit keyring fixture")));
    auto require_empty = [](const DecodeResult& result, Status status, const char* description) {
        require(result.status == status && !result.evidence &&
                result.classification == "unknown LoRa" && result.authentication == "not authenticated", description);
    };
    require_empty(classify_meshtastic(frame, true, std::span<const Profile>{}),
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
    require_empty(classify_meshtastic(frame, true, profiles), Status::decryption_unvalidated,
                  "matching hash without valid decryption does not identify mesh traffic");
    auto selected = synthetic_profile(); selected.id = "authorized-channel";
    profiles.push_back(std::move(selected));
    auto result = classify_meshtastic(frame, true, profiles);
    require(result.status == Status::classified && result.evidence && result.evidence->port == 1 &&
            result.classification == "likely Meshtastic" && result.authentication == "not authenticated",
            "keyring selects explicit channel independently of any receiver lane");
    std::reverse(profiles.begin(), profiles.end());
    result = classify_meshtastic(frame, true, profiles);
    require(result.evidence && result.evidence->port == 1, "classification independent of profile ordering");

    const auto unsupported_frame = frame_for(source, envelope(256, {1,2,3,4}));
    result = classify_meshtastic(unsupported_frame, true, profiles);
    require(result.status == Status::unsupported_payload && result.evidence &&
            result.evidence->port == 256 && result.classification == "possible Meshtastic",
            "unique keyring unsupported envelope retains only bounded evidence");

    auto alias = synthetic_profile(); alias.id = "same-key-different-channel"; alias.channel_name += "AA";
    require(alias.channel_name != source.channel_name && channel_hash(alias.channel_name, alias.keys.front()) == frame[13],
            "distinct channel names can share the same key and weak hash");
    profiles.push_back(std::move(alias));
    result = classify_meshtastic(frame, true, profiles);
    require(result.evidence && result.evidence->port == 1,
            "same-key aliases decode without inventing unique channel provenance");
    result = classify_meshtastic(unsupported_frame, true, profiles);
    require(result.status == Status::unsupported_payload && result.evidence && result.evidence->port == 256,
            "duplicate unsupported envelopes preserve bounded evidence");
    std::reverse(profiles.begin(), profiles.end());
    result = classify_meshtastic(frame, true, profiles);
    require(result.evidence && result.evidence->port == 1,
            "duplicate alias classification is order independent");
    require_empty(classify_meshtastic(frame, false, profiles), Status::bad_phy_crc,
                  "bad PHY CRC suppresses every keyring attempt");

    profiles.clear(); profiles.push_back(synthetic_profile());
    auto invalid = synthetic_profile(); invalid.id = "invalid-later-profile";
    auto moved = std::move(invalid.keys.front());
    profiles.push_back(std::move(invalid));
    require_empty(classify_meshtastic(frame, true, profiles), Status::invalid_profile,
                  "invalid later key record prevents partial keyring success");
    profiles.back() = synthetic_profile(); profiles.back().channel_name.clear();
    require_empty(classify_meshtastic(frame, true, profiles), Status::invalid_profile,
                  "invalid channel name prevents every attempt");
    profiles.clear();
    for (std::size_t index = 0; index < max_keyring_profiles; ++index) {
        auto profile = synthetic_profile(); profile.id = "configured-" + std::to_string(index);
        profile.keys.clear(); profiles.push_back(std::move(profile));
    }
    require_empty(classify_meshtastic(frame, true, profiles), Status::no_matching_key,
                  "maximum valid keyless profiles remain bounded");
    profiles.push_back(synthetic_profile());
    require_empty(classify_meshtastic(frame, true, profiles), Status::invalid_profile,
                  "excessive profile count rejected before successful key");
    profiles.clear(); profiles.push_back(synthetic_profile());
    for (std::size_t index = 0; index < max_profile_keys; ++index)
        profiles.front().keys.push_back(std::move(*ChannelKey::from_hex("000102030405060708090a0b0c0d0e0f")));
    require_empty(classify_meshtastic(frame, true, profiles), Status::invalid_profile,
                  "excessive keys in a profile rejected before successful key");

    const auto single = classify_meshtastic(frame, true, source);
    const auto span = classify_meshtastic(frame, true, std::span<const Profile>(&source, 1));
    require(single.status == span.status && single.classification == span.classification &&
            single.evidence && span.evidence && single.evidence->port == span.evidence->port,
            "single-profile wrapper preserves classification evidence");

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
        result = classify_meshtastic(candidate_frame, true, profiles);
        require(result.evidence && result.evidence->port == 1 && result.authentication == "not authenticated",
                "explicit key-only classification is independent of channel name and hash");
    }
    const auto unrelated_key_frame = [&] {
        auto profile = synthetic_profile(); profile.keys.clear();
        profile.keys.push_back(std::move(*ChannelKey::from_hex("101112131415161718191a1b1c1d1e1f")));
        return frame_for(profile, envelope(1, string_bytes("Other key fixture")));
    }();
    require_empty(classify_meshtastic(unrelated_key_frame, true, profiles), Status::decryption_unvalidated,
                  "key-only discovery never adds an unrelated key or retains wrong-key bytes");
    require_empty(classify_meshtastic(frame, false, profiles), Status::bad_phy_crc,
                  "key-only profile still requires valid PHY CRC");
    result = classify_meshtastic(unsupported_frame, true, profiles);
    require(result.status == Status::unsupported_payload && result.evidence && result.evidence->port == 256,
            "key-only unsupported envelopes expose bounded evidence only");
    auto restricted_alias = synthetic_profile(); restricted_alias.id = "named-alias";
    profiles.push_back(std::move(restricted_alias));
    result = classify_meshtastic(frame, true, profiles);
    require(result.evidence && result.evidence->port == 1,
            "key-only and named aliases preserve classification evidence");
    result = classify_meshtastic(other_frame, true, profiles);
    require(result.evidence && result.evidence->port == 1,
            "ineligible named alias does not affect key-only classification");
    profiles.pop_back(); profiles.front().restrict_channel_name = true;
    require_empty(classify_meshtastic(frame, true, profiles), Status::invalid_profile,
                  "empty name needs an explicit key-only choice");
    profiles.front().restrict_channel_name = false; profiles.front().keys.clear();
    require_empty(classify_meshtastic(frame, true, profiles), Status::no_matching_key,
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
    require(classify_meshtastic(short_frame, true, first).evidence.has_value() &&
            classify_meshtastic(short_frame, true, second).evidence.has_value(),
            "distinct fixture keys each produce plausible structure alone");
    profiles.push_back(std::move(first)); profiles.push_back(std::move(second));
    require_empty(classify_meshtastic(short_frame, true, profiles), Status::ambiguous_keys,
                  "different plausible key material still suppresses content and evidence");
    std::reverse(profiles.begin(), profiles.end());
    require_empty(classify_meshtastic(short_frame, true, profiles), Status::ambiguous_keys,
                  "distinct-key ambiguity remains order independent");
}
void envelope_evidence() {
    auto profile = synthetic_profile();
    auto classify = [&](const Bytes& data) { return classify_meshtastic(frame_for(profile, data), true, profile); };
    Bytes request; scalar(request,1,70); scalar(request,3,1);
    auto result = classify(request);
    require(result.evidence && result.evidence->port==70 && !result.evidence->signature_present,
            "empty traceroute envelope may omit payload without reading route contents");
    Bytes no_payload; scalar(no_payload,1,1);
    require(!classify(no_payload).evidence,"other application envelopes require a payload field");
    for (unsigned size : {0u,63u,64u,65u}) {
        auto data=envelope(1,string_bytes("Synthetic signature shape"));blob(data,10,Bytes(size,0x5a));
        result=classify(data);
        require(size==0 || size==64 ? result.evidence && result.evidence->signature_present==(size==64) : !result.evidence,
                "signature length bounded; presence never implies verification");
    }
    auto data=envelope(1,string_bytes("Future envelope"));scalar(data,123,42);
    require(classify(data).evidence.has_value(),"bounded unknown envelope field safely skipped");
    result=classify(envelope(256,{1,2,3,4}));
    require(result.evidence && result.evidence->port==256 && result.status==Status::unsupported_payload &&
            result.classification=="possible Meshtastic" && result.authentication=="not authenticated",
            "unrecognized port retains only bounded evidence");
    for (unsigned port : {0u,65536u}) require(!classify(envelope(port,{1})).evidence,"port bounds");
    require(!classify(envelope(1,Bytes(234,0xaa))).evidence,"opaque payload size still bounded");
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
        auto r = classify_meshtastic(frame_for(profile, plain), true, profile);
        if (!r.evidence || r.evidence->port != fixture.port || r.evidence->signature_present != fixture.signature)
            throw std::runtime_error("Official fixture evidence failed: " + std::string(fixture.name));
        require(r.authentication == "not authenticated", "official serializer cannot authenticate sender");
        const bool known = fixture.port==1 || fixture.port==3 || fixture.port==4 || fixture.port==5 || fixture.port==67 || fixture.port==70;
        require(r.status==(known?Status::classified:Status::unsupported_payload), "official envelope classification");
    }
}
void public_channel_scope() {
    auto receiver = public_meshtastic_profile();
    require(!receiver.restrict_channel_name && receiver.channel_name.empty() && receiver.keys.size() == 1,
        "Public default is one survey key independent of preset or channel name");
    const auto expected = ChannelKey::from_user_input("AQ==");
    require(std::equal(receiver.keys.front().bytes().begin(), receiver.keys.front().bytes().end(), expected->bytes().begin()),
        "Public default resolves the published index-one key");
    std::vector<std::string_view> channel_names{"CustomName"};
    for (const auto& preset : ovmesh::meshtastic::presets) channel_names.push_back(preset.name);
    for (const auto name : channel_names) {
        auto sender = public_meshtastic_profile();
        sender.channel_name = std::string(name);
        const auto frame = frame_for(sender, envelope(1, string_bytes("Public channel fixture")));
        require(classify_meshtastic(frame, true, receiver).status == Status::classified,
            "Public default classifies valid envelopes regardless of preset-derived channel names");
        require(classify_meshtastic(frame, false, receiver).status == Status::bad_phy_crc,
            "Public default cannot bypass failed radio integrity checks");
        require(classify_meshtastic(frame, true, std::span<const Profile>{}).status == Status::no_matching_key,
            "An empty caller keyring still opts out of public-key classification");
    }
    const auto unrelated = frame_for(synthetic_profile(), envelope(1, string_bytes("Private channel fixture")));
    require(classify_meshtastic(unrelated, true, receiver).status == Status::decryption_unvalidated,
        "Public default does not classify a frame encrypted with an unrelated private key");
    require(status_explanation(Status::bad_phy_crc).find("before any channel key") != std::string_view::npos &&
        status_explanation(Status::no_matching_key).find("eligible") != std::string_view::npos &&
        status_explanation(Status::decryption_unvalidated).find("keys were tried") != std::string_view::npos,
        "Diagnostics distinguish RF integrity, eligible-key absence and failed envelope validation");
}
}
int main() {
    try { explicit_key_input(); protocol_vectors(); opaque_payloads(); envelope_evidence(); independent_official_fixtures(); rejection_and_bounds(); frequency_independent_keyring(); public_channel_scope(); std::cout<<"protocol: explicit key input, independent crypto, official 2.7.19/2.8.0 fixtures, opaque payloads, envelope evidence, frequency-independent keyring, public-channel scope, authorization and bounds checks passed\n"; return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
