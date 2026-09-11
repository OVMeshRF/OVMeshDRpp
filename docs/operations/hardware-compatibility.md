# Receiver and platform compatibility

OVMeshDR++ provides direct, receive-only USB adapters for **HackRF One and RTL-SDR**, plus an experimental **RAK5146 USB/LBT** concentrator adapter. Select the receiver in the main dropdown while stopped. HackRF and RTL-SDR use the spectrum/waterfall workflow. RAK uses sampled RSSI scans and configured packet reception, with its own recording and reporting semantics. Optional receiver GPS is a separate input. This is experimental source support, with the validation limits below.

## Receiver capabilities in this build

| Capability | HackRF One | RTL-SDR |
|---|---|---|
| Application sample rates | 8, 10, 12, 16 or 20 MS/s | 1 or 2 MS/s |
| Maximum configured spectrum span | 80% of sample rate; up to 16 MHz at 20 MS/s | 0.8 MHz at 1 MS/s; 1.6 MHz at 2 MS/s |
| Experimental LoRa discovery | Available within supported range/rate settings; processing capacity must be checked | Requires 2 MS/s and a span no wider than 1.5 MHz |
| Main gain controls | LNA, VGA and RF amplifier | Tuner gain, manual or automatic; actual applied manual gain is recorded |
| Receive adapter | Existing libhackrf and libusb | Pinned minimal librtlsdr source built against libusb |
| CMake option | `OVMESH_ENABLE_HACKRF` | `OVMESH_ENABLE_RTLSDR` |
| Current hardware evidence | Local macOS reception and survey checks | Local macOS R820T reception, recording and waveform-observation checks |

These spans are application limits, not measured guarantees of a flat or interference-free passband. FFT edge rounding and the receiver-center guard leave explicitly unassessed frequencies. A 16 MHz HackRF survey remains a performance/passband target to qualify. RTL-SDR rates above 2 MS/s and wider swept surveys are not implemented. Changing receiver settings starts a new session; no single RTL survey observes the entire 902–928 MHz band.

RTL tuning is restricted by this application to 24–1766 MHz, including the survey edges and corrected center. Actual coverage also depends on the attached tuner. One tested R820T does not establish compatibility with every RTL model or tuner. Receiver Offset and serial selection reset when switching hardware types. See [RTL-SDR setup and troubleshooting](rtl-sdr.md) and the [user guide](../user-guide.md#configure-a-spectrum-receiver).

## RAK5146 USB/LBT

The initial adapter targets the **US915 USB/LBT RAK5146-126**, using one or two selected boards. Other RAK variants, SPI carriers and concentrator models are not validated by this support claim.

| Capability | Experimental RAK5146 adapter |
|---|---|
| Sampled scan range | Configurable range within 902–928 MHz; scan-filter edges must fit within the selected range |
| Scan method | Auxiliary SX1261 RSSI histogram: 33 counters, 2,000 samples per completed scan, nominal 234.3 kHz filter |
| Scan spacing | 25–1000 kHz; two boards split the assigned scan centers |
| Packet reception | One configured service modem per board: 125/250/500 kHz, SF7–12, sync word 0x2B, 0x12 or 0x34 |
| Two-board use | Independent packet profiles, for example LongFast and LongTurbo, alongside assigned scan centers |
| Gain / power | Nominal vendor RSSI scale; no SDR LNA/VGA controls or transmitter-power measurement |
| Driver | Pinned minimal Semtech HAL, one isolated local receive-only worker per USB device |
| Build option | `OVMESH_ENABLE_RAK5146`; no packet forwarder, network service or firmware flasher |
| Validation | macOS application checks passed with one and two boards scanning while receiving configured packets; a visible two-board desktop check saved CRC-valid LongFast and LongTurbo receptions. Endurance and Linux hardware qualification remain pending |

This mode does not produce IQ, FFT bins or an SDR waterfall. RSSI sample exceedance is not continuous busy time, channel utilization or packet airtime. The scanner misses activity between visits and does not classify its energy by protocol. Packet bandwidth comes from the configured service modem, not an independent measurement of transmitter width. Reception requires a configured frequency/BW/SF; this is not automatic all-frequency/all-preset packet discovery. Sync-word matches and valid CRCs do not authenticate a sender or prove a mesh protocol.

The worker interleaves packet polling and auxiliary scanning on one connection, checks initialization/readback, and stops on transport or framing failure. The application must retain `ovmesh-rak-worker` beside its executable; macOS bundle and install rules include it. It never requests MCU bootloader mode or implements RF transmission. No carrier GPS or PPS integration is inferred from USB connectivity. See [RAK setup](rak5146.md), [concentrator measurements and reports](../design/concentrator-measurements.md) and [source provenance](../../third_party/sx1302_hal/PROVENANCE.md).

## Platform status

| Platform | Status |
|---|---|
| macOS / Apple Silicon | HackRF/RTL development builds and local USB reception checks; RAK integrated one/two-board scan, packet, recording and export checks. No signed/notarized redistributable package |
| Linux | HackRF/RTL source targets the platform. RAK requires glibc 2.34+ and safe child-process descriptor closure; unsupported builds disable the RAK worker. Platform compilation, USB permissions, GPS and live receiver acceptance remain qualification work |
| Windows | HackRF/RTL source targets the platform; compilation, driver binding, GPS and live receiver acceptance remain qualification work. RAK worker is disabled; no Windows RAK support is claimed |

Build dependencies and native USB requirements are described in [deployment](deployment.md). A missing adapter at build time is different from a connected device being unavailable or busy. The application does not install system drivers or change USB permissions automatically.

## Shared measurement and decoding limits

HackRF and RTL-SDR report uncalibrated received dBFS; RAK reports nominal vendor RSSI in dBm. Neither establishes transmitter watts. Gain, antenna, threshold, clipping and observation coverage affect comparisons. Spectrum coverage and LoRa discovery loss are separate measurements. Neither a moving waterfall, completed sweep nor a successful detection proves complete capture.

The RTL and RAK adapters do not change protocol maturity: SDR LoRa waveform discovery is experimental, automatic discovered-waveform dispatch to payload decoding remains unfinished, selected-profile Meshtastic decoding remains experimental, and MeshCore decoding is not implemented. Broader model, platform, GPS-concurrency and endurance testing remain necessary. See [validation scope](../engineering/quality-and-validation.md) and [RF capability gaps](../engineering/rf-survey-gap-analysis.md).
