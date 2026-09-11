// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ovmesh/concentrator.hpp"
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace ovmesh {
struct RakMessage {
    enum class Kind { Ready, Heartbeat, Scan, Packet, End, Error } kind;
    ConcentratorScan scan;
    uint64_t frequency_hz = 0;
    unsigned bandwidth_hz = 0, spreading_factor = 0, coding_rate = 0;
    double rssi_dbm = 0, snr_db = 0;
    bool crc_valid = false;
    uint32_t hardware_timestamp_us = 0;
    double scan_monotonic_start = 0, scan_monotonic_end = 0;
    std::vector<uint8_t> payload;
    // Fixed, sanitized diagnostic only; never worker input or packet bytes.
    std::string error;
    ~RakMessage();
    RakMessage() = default;
    RakMessage(RakMessage&&) noexcept = default;
    RakMessage& operator=(RakMessage&&) noexcept = delete;
    RakMessage(const RakMessage&) = delete;
};
// Pure bounded parser. Throws generic errors with no input bytes in diagnostics.
RakMessage parse_rak_message(std::string_view);
class RakProcess {
public:
    RakProcess();
    ~RakProcess();
    RakProcess(const RakProcess&) = delete;
    void start(const ConcentratorBoardConfig&, int64_t offset_hz,
               uint64_t scan_lower_hz, uint64_t scan_upper_hz,
               uint32_t scan_step_hz, unsigned scan_samples, unsigned board_index);
    // One bounded read. Valid records preceding a terminal Error are returned
    // first, so an error cannot discard already completed measurements.
    std::vector<RakMessage> poll();
    // STOP, then bounded drain through the ordinary record consumer. A clean
    // stop requires END, exit status zero and no rejected or discarded data.
    bool stop(const std::function<void(RakMessage&)>& consume = {}) noexcept;
    bool alive() const;
    static bool available() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
double rak_monotonic_now();
}
