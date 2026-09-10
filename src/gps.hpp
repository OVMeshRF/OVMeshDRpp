// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ovmesh/engine.hpp"
#include <functional>
#include <memory>
#include <string_view>
#include <span>

namespace ovmesh {
// Parses a complete checksummed RMC/GGA sentence. A bad/invalid fix never becomes valid.
std::optional<PositionFix> parse_nmea(std::string_view sentence, double received_monotonic,
                                     double received_utc);
// Ordered history includes invalid-fix/clear markers. A later loss of GPS must
// not erase a valid association for earlier sample time; no fix crosses a marker.
std::optional<PositionFix> position_at(std::span<const PositionFix> history, double observed_monotonic);
class SerialGps {
public:
    SerialGps();
    ~SerialGps();
    bool start(const std::string&, unsigned baud, std::function<void(PositionFix)> callback,
               std::string& error);
    void stop();
    GpsConnectionStatus status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
double monotonic_now();
double utc_now();
}
