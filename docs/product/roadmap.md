# Roadmap

The priority is reliable range-wide measurement, local recording and meaningful frequency/time/location reporting. Protocol identification and channel recommendations build on that evidence; they are not prerequisites for observing RF activity.

## Current experimental capability

Version 0.4.0 provides HackRF and RTL-SDR receivers, native spectrum/waterfall, fine activity records, compact storage, GPS association, saved analysis, filtered exports and HTML findings reports. Experimental one/two-board RAK5146 USB/LBT support adds sampled RSSI sweeps and configured packet reception with separate measurement records and reports. Experimental LoRa waveform discovery and explicit-profile Meshtastic decoding remain separate paths. See [implementation status](../engineering/implementation-status.md) and [validation scope](../engineering/quality-and-validation.md).

## Near-term work

| Work | Outcome | Gate |
|---|---|---|
| Receiver and recording reliability | Predictable continuous capture, truthful coverage/loss reporting and recoverable sessions | Sustained load, disconnection/storage failures and supported-configuration matrix |
| Additional receiver support | RTL-SDR adapter implemented with the same measurement/retention contracts at 1/2 MS/s | More tuner models, extended endurance and platform-specific validation; separate design needed for wider swept surveys |
| Concentrator qualification | RAK5146 one/two-board scans and configured packet reception implemented on macOS and supported Linux builds | Sustained operation, USB recovery, Linux hardware, calibrated RSSI and moving-GPS testing; Windows worker remains unimplemented |
| Measurement characterization | Defensible occupancy/power comparisons across settings and receivers | Calibrated source checks, passband/threshold characterization, overload and uncertainty reporting |
| Regional analysis | Compare preserved sessions and repeat visits by frequency, time and receiver area | Setup compatibility, missing-data handling and source provenance preserved in comparisons |
| Survey/report usability | Clear observation coverage and manageable reports for regional operators | Review against representative surveys without changing the meaning of stored measurements |

The [RTL-SDR adapter](../operations/rtl-sdr.md) and [RAK5146 adapter](../operations/rak5146.md) are implemented with explicit limits. This is not a guarantee for every device, operating system or USB layout. RAK currently uses one service modem per board; the eight 125 kHz multi-SF lanes and automatic profile scheduling require separate work. The remaining work is ordered by evidence and dependencies rather than committed delivery dates.

## Full-range protocol discovery

Discover compatible traffic anywhere within the supplied usable span without known frequency, BW or SF. The current waveform detector supplies inferred settings but does not automatically dispatch payload decoding. Complete that connection using the explicit survey-wide keyring and independently reported acquisition, discovery and decoder coverage.

Acceptance requires labeled independent fixtures and controlled measurements covering sensitivity, timing, overlaps, false classification and overload. An empty decode list must never be presented as empty spectrum. Approximately 16 MHz recording throughput and 16 MHz exhaustive decoding are separate claims.

## MeshCore and other classification

Add native MeshCore framing, authorized key handling, bounded content mapping and protocol-specific fixtures. Its PHY, packet and encryption semantics require separate review. Reliable LoRaWAN/other-device attribution needs its own evidence; a LoRa waveform, frequency or sync word alone is insufficient.

Recipient-private-key messages and stronger protocol identity verification remain separate security and compatibility work. No classification feature may silently broaden content retention.

## Later analysis and access

Candidate-frequency recommendations should explain exposure, setup comparability, uncertainty and competing activity. They cannot declare a channel compliant or interference-free. Multi-receiver comparison and optional wider scanning follow the same coverage requirements.

Offline map integration and remote/mobile monitoring are later possibilities. Remote access would need explicit authentication, data permissions and security review; there is no current cloud dependency or listener.

## Release requirements

Every supported feature needs current documentation, relevant correctness/robustness tests, clear capacity and platform limits, and reviewed dependency/license provenance. Binary distribution additionally needs packaging, signing, clean-machine validation and realistic support commitments. See [release engineering](../engineering/development-and-releases.md) and the [capability gaps](../engineering/rf-survey-gap-analysis.md).
