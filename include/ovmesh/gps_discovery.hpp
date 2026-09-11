// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ovmesh {

struct GpsDevice {
    std::string path;
    std::string label;
    // A USB serial identity when provided, otherwise explicitly port/location scoped.
    // Device metadata is descriptive, not proof of hardware authenticity.
    std::string stable_id;
    bool automatic_candidate = false;
};

struct GpsDiscovery {
    std::vector<GpsDevice> devices;
    std::string error;
};

// Normalized OS registry/sysfs metadata; classification is pure and never probes a port.
struct SerialDeviceMetadata {
    std::string path;
    std::string product;
    std::string manufacturer;
    std::string serial;
    std::string location;
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
};

std::optional<GpsDevice> classify_gps_serial_device(const SerialDeviceMetadata& metadata);

// Enumerates metadata only. Does not open serial ports, change settings or write bytes.
GpsDiscovery discover_gps_devices();

// The Semtech USB bridge uses a generic STM32 VID/PID shared by unrelated
// products. These are candidates for an explicit operator choice, never proof
// of a RAK board and never an instruction to open a serial port.
struct ConcentratorDevice {
    std::string path;
    std::string label;
    std::string stable_id;
};
struct ConcentratorDiscovery {
    std::vector<ConcentratorDevice> devices;
    std::string error;
};
std::optional<ConcentratorDevice> classify_concentrator_serial_device(const SerialDeviceMetadata& metadata);
ConcentratorDiscovery discover_concentrator_devices();
// An empty, missing or duplicate identity never selects a board automatically.
std::optional<std::size_t> select_concentrator_device(const std::vector<ConcentratorDevice>& devices,
                                                    const std::string& preferred_id);

// A missing or duplicated remembered identity never falls back to another device.
// Without a preference, only one unambiguous recognized GPS may be selected.
std::optional<std::size_t> select_gps_device(const std::vector<GpsDevice>& devices,
                                          const std::string& preferred_id);

} // namespace ovmesh
