// SPDX-License-Identifier: GPL-3.0-or-later
#include "ovmesh/concentrator.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace ovmesh {
void validate_concentrator_config(const ConcentratorConfig& c, uint64_t center,
                                 uint32_t span, int64_t offset, bool require_paths) {
    if(c.boards.empty() || c.boards.size()>2)
        throw std::runtime_error("Choose one or two RAK5146 concentrators");
    if(center<902000000 || center>928000000 || span<500000 || span>26000000 ||
       offset< -100000 || offset>100000)
        throw std::runtime_error("RAK5146 US915 survey requires 902–928 MHz, a 0.5–26 MHz span and Offset within +/-100 kHz");
    const double lower=double(center)-span/2., upper=double(center)+span/2.;
    if(lower<902000000 || upper>928000000 || lower+offset<902000000 || upper+offset>928000000)
        throw std::runtime_error("Requested and corrected RAK5146 survey edges must remain within 902–928 MHz");
    if(c.scan_step_hz<25000 || c.scan_step_hz>1000000 || c.scan_samples!=2000)
        throw std::runtime_error("RAK scan step must be 25–1000 kHz, with 2000 RSSI samples per histogram");
    for(size_t i=0;i<c.boards.size();++i) {
        const auto& b=c.boards[i];
        if(require_paths) {
            if(b.device_path.empty() || b.device_path.size()>1024 || b.device_id.size()>2048 ||
               b.device_path.find('\0')!=std::string::npos || b.device_id.find('\0')!=std::string::npos)
                throw std::runtime_error("Select each concentrator from the local USB device list");
            if(i && (b.device_path==c.boards[0].device_path ||
                (!b.device_id.empty() && b.device_id==c.boards[0].device_id)))
                throw std::runtime_error("Two concentrators must be two different USB devices");
        }
        if(!b.packets_enabled && !c.scan_enabled)
            throw std::runtime_error("Enable sampled RF scanning or packet reception for each concentrator");
        if(b.bandwidth_hz!=125000 && b.bandwidth_hz!=250000 && b.bandwidth_hz!=500000)
            throw std::runtime_error("RAK service modem bandwidth must be 125, 250 or 500 kHz");
        if(b.spreading_factor<7 || b.spreading_factor>12 ||
           (b.sync_word!=0x2b && b.sync_word!=0x12 && b.sync_word!=0x34))
            throw std::runtime_error("RAK service modem supports SF7–12 and sync words 0x2B, 0x12 or 0x34");
        if(b.frequency_hz<902000000 || b.frequency_hz>928000000)
            throw std::runtime_error("RAK packet frequency must be within 902–928 MHz");
        if(b.packets_enabled && (double(b.frequency_hz)-b.bandwidth_hz/2.<lower ||
                double(b.frequency_hz)+b.bandwidth_hz/2.>upper))
            throw std::runtime_error("Each enabled packet profile's full bandwidth must fit inside the survey range");
    }
}
uint64_t concentrator_sample_count(const ConcentratorScan& s) {
    return std::accumulate(s.counts.begin(),s.counts.end(),uint64_t{});
}
double concentrator_bin_lower_dbm(unsigned bin, double offset) {
    if(bin>32 || !std::isfinite(offset))throw std::invalid_argument("Invalid RSSI histogram bin");
    return bin==32 ? -std::numeric_limits<double>::infinity() : offset-4.*bin;
}
double concentrator_fraction_above(const ConcentratorScan& s, double threshold) {
    if(!std::isfinite(threshold) || !std::isfinite(s.rssi_offset_db))
        throw std::invalid_argument("Invalid RSSI histogram threshold");
    uint64_t above=0;
    for(unsigned i=0;i<32;++i)if(concentrator_bin_lower_dbm(i,s.rssi_offset_db)>=threshold)above+=s.counts[i];
    const auto total=concentrator_sample_count(s);
    return total ? double(above)/double(total) : 0;
}
}
