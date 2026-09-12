// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/meshtastic_presets.hpp"
#include "ovmesh/phy.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using namespace ovmesh::meshtastic;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void schema_completeness(const char* version, std::size_t expected_count, bool older) {
    // Read the already vendored official schema; no generator or network use.
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    std::ifstream source(root / "third_party/meshtastic/schemas" / version / "meshtastic/config.proto");
    require(source.good(), "Read vendored official preset schema");
    const std::regex entry(R"(^\s*([A-Z_]+)\s*=\s*([0-9]+)(.*);)");
    std::set<unsigned> found;
    bool inside = false;
    std::string line;
    while (std::getline(source, line)) {
        if (!inside) {
            if (line.find("enum ModemPreset {") != std::string::npos) inside = true;
            continue;
        }
        if (line.find('}') != std::string::npos) break;
        std::smatch match;
        if (!std::regex_search(line, match, entry)) continue;
        const auto id = static_cast<unsigned>(std::stoul(match[2]));
        require(id < 256, "Wire preset ID fits public catalog type");
        const auto* preset = find_preset(static_cast<PresetId>(id));
        require(preset && preset->wire_name == match[1].str(), "Every schema preset has its correct stable ID/name");
        require(preset->deprecated == (match[3].str().find("deprecated = true") != std::string::npos),
            "Catalog deprecation matches official schema");
        require(!older || preset->available_in_2_7_19, "Older schema availability remains explicit");
        require(found.insert(id).second, "Schema preset IDs are unique");
    }
    require(found.size() == expected_count, "Schema preset count matches independently reviewed baseline");
    for (const auto& preset : presets) {
        require(found.contains(static_cast<unsigned>(preset.id)) == (!older || preset.available_in_2_7_19),
            "Catalog has no invented preset for a reviewed schema version");
    }
}

void parameter_vectors() {
    // Frozen RF parameter vectors from the cited upstream switches, in stable
    // protobuf ID order. VeryLongSlow uses its documented pre-2.5 RF profile.
    constexpr std::array<std::array<unsigned, 3>, 17> expected{{
        {250000,11,5}, {125000,12,8}, {62500,12,8}, {250000,10,5},
        {250000,9,5}, {250000,8,5}, {250000,7,5}, {125000,11,8},
        {500000,7,5}, {500000,11,8}, {125000,9,5}, {125000,10,5},
        {62500,7,6}, {62500,8,6}, {15625,7,5}, {15625,8,6}, {500000,9,5}
    }};
    std::set<std::string_view> names;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto* preset = find_preset(static_cast<PresetId>(i));
        require(preset && preset->bandwidth_hz == expected[i][0] &&
            preset->spreading_factor == expected[i][1] &&
            preset->coding_rate_denominator == expected[i][2], "Official RF parameter vector");
        require(!preset->name.empty() && names.insert(preset->name).second, "Unique visible preset names");
        require(preset->phy_supported && ovmesh::supported_lora_bandwidth(preset->bandwidth_hz),
            "Every advertised preset is accepted by the native PHY bandwidth contract");
        require(preset->historical_only == (i == 2), "Only removed VeryLongSlow uses historical RF parameters");
    }
    require(!find_preset(static_cast<PresetId>(255)), "Unknown preset never aliases LongFast");
    require(public_channel_key_token == "AQ==", "One explicit public channel-key token for the whole survey");
}

void all_preset_phy_acceptance() {
    // Construct and exercise every advertised parameter bundle, including LDRO
    // at narrow bandwidths. This validates clean synthetic PHY operation only.
    constexpr std::array<std::uint8_t, 12> data{3,17,41,65,83,101,127,151,179,197,223,251};
    for (const auto& preset : presets) {
        const ovmesh::PhyConfig config{preset.bandwidth_hz, preset.spreading_factor,
                                      preset.coding_rate_denominator, 0x2b};
        auto samples = ovmesh::modulate_lora(data, config);
        samples.resize(samples.size() + (1U << config.spreading_factor) * 2);
        ovmesh::LoRaReceiver receiver(config);
        unsigned received = 0;
        receiver.feed(samples, [&](ovmesh::PhyFrame&& frame) {
            require(frame.header_valid && frame.payload_crc_present && frame.payload_crc_valid,
                "Every catalog preset must decode clean synthetic PHY CRC");
            require(frame.bytes.size() == data.size() &&
                std::equal(frame.bytes.begin(), frame.bytes.end(), data.begin()), "Synthetic PHY bytes preserved");
            require(frame.coding_rate == preset.coding_rate_denominator, "Actual decoded coding rate matches fixture");
            ++received;
        });
        if (received != 1) throw std::runtime_error("Synthetic PHY acceptance failed for " + std::string(preset.name));
    }
}
} // namespace

int main() {
    try {
        schema_completeness("v2.7.19", 10, true);
        schema_completeness("v2.8.0", 17, false);
        parameter_vectors();
        all_preset_phy_acceptance();
        std::cout << "Meshtastic catalog: all 17 reviewed presets and clean synthetic PHY checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
