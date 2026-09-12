// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ovmesh::meshtastic {

// Stable Config.LoRaConfig.ModemPreset wire IDs, including deprecated values.
// RF parameters describe sub-GHz LoRa, not the different 2.4 GHz wide-LoRa modes.
// Source revisions and the historical VeryLongSlow exception are documented in
// docs/research/meshtastic-presets.md. This is not a channel/frequency whitelist.
enum class PresetId : std::uint8_t {
    LongFast = 0, LongSlow = 1, VeryLongSlow = 2, MediumSlow = 3,
    MediumFast = 4, ShortSlow = 5, ShortFast = 6, LongModerate = 7,
    ShortTurbo = 8, LongTurbo = 9, LiteFast = 10, LiteSlow = 11,
    NarrowFast = 12, NarrowSlow = 13, TinyFast = 14, TinySlow = 15,
    MediumTurbo = 16
};

struct Preset {
    PresetId id;
    std::string_view name;
    std::uint32_t bandwidth_hz;
    std::uint8_t spreading_factor;
    std::uint8_t coding_rate_denominator;
    bool deprecated;
    // Parameter support in the native SDR PHY; not proof of on-air reception,
    // recognition completeness, regional permission, or concentrator support.
    bool phy_supported;
    std::string_view support_note;
    bool historical_only;
    bool available_in_2_7_19;
    std::string_view wire_name;
};

inline constexpr std::array<Preset, 17> presets{{
    {PresetId::LongFast, "LongFast", 250000, 11, 5, false, true, "", false, true, "LONG_FAST"},
    {PresetId::LongSlow, "LongSlow", 125000, 12, 8, true, true,
        "Deprecated by upstream; retained for existing traffic.", false, true, "LONG_SLOW"},
    {PresetId::VeryLongSlow, "VeryLongSlow", 62500, 12, 8, true, true,
        "Historical RF profile; current firmware no longer maps this enum to these parameters.",
        true, true, "VERY_LONG_SLOW"},
    {PresetId::MediumSlow, "MediumSlow", 250000, 10, 5, false, true, "", false, true, "MEDIUM_SLOW"},
    {PresetId::MediumFast, "MediumFast", 250000, 9, 5, false, true, "", false, true, "MEDIUM_FAST"},
    {PresetId::ShortSlow, "ShortSlow", 250000, 8, 5, false, true, "", false, true, "SHORT_SLOW"},
    {PresetId::ShortFast, "ShortFast", 250000, 7, 5, false, true, "", false, true, "SHORT_FAST"},
    {PresetId::LongModerate, "LongModerate", 125000, 11, 8, false, true, "", false, true, "LONG_MODERATE"},
    {PresetId::ShortTurbo, "ShortTurbo", 500000, 7, 5, false, true, "", false, true, "SHORT_TURBO"},
    {PresetId::LongTurbo, "LongTurbo", 500000, 11, 8, false, true, "", false, true, "LONG_TURBO"},
    {PresetId::LiteFast, "LiteFast", 125000, 9, 5, false, true, "", false, false, "LITE_FAST"},
    {PresetId::LiteSlow, "LiteSlow", 125000, 10, 5, false, true, "", false, false, "LITE_SLOW"},
    {PresetId::NarrowFast, "NarrowFast", 62500, 7, 6, false, true, "", false, false, "NARROW_FAST"},
    {PresetId::NarrowSlow, "NarrowSlow", 62500, 8, 6, false, true, "", false, false, "NARROW_SLOW"},
    {PresetId::TinyFast, "TinyFast", 15625, 7, 5, false, true,
        "15.625 kHz modem bandwidth; upstream's 20 kHz description includes channel padding.",
        false, false, "TINY_FAST"},
    {PresetId::TinySlow, "TinySlow", 15625, 8, 6, false, true,
        "15.625 kHz modem bandwidth; upstream's 20 kHz description includes channel padding.",
        false, false, "TINY_SLOW"},
    {PresetId::MediumTurbo, "MediumTurbo", 500000, 9, 5, false, true, "", false, false, "MEDIUM_TURBO"}
}};

constexpr const Preset* find_preset(PresetId id) noexcept {
    for (const auto& preset : presets) if (preset.id == id) return &preset;
    return nullptr;
}

// A preset is an RF parameter bundle, never an encryption-key identity.
// This single public token is expanded only by the explicit survey key setup.
inline constexpr std::string_view public_channel_key_token = "AQ==";

} // namespace ovmesh::meshtastic
