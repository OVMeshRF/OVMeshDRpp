# Receiver and platform compatibility

OVMeshDR++ provides direct, receive-only USB adapters for **HackRF One and RTL-SDR**, plus an experimental **RAK5146 USB/LBT** concentrator adapter. **HackRF One is the recommended hardware for wider RF surveys**, with the wider configured span shown below. Select the receiver in the main dropdown while stopped. HackRF and RTL-SDR use the spectrum/waterfall workflow. RAK uses sampled RSSI scans, with its own recording and reporting semantics. Optional receiver GPS is a separate input. This is experimental source support, with the validation limits below.

## Receiver capabilities in this build

| Capability | HackRF One | RTL-SDR |
|---|---|---|
| Application sample rates | 8, 10, 12, 16 or 20 MS/s | 1 or 2 MS/s |
| Default survey setup | 10 MHz span at 16 MS/s | 1.5 MHz span at 2 MS/s |
| Maximum configured spectrum span | 80% of sample rate; up to 16 MHz at 20 MS/s | 0.8 MHz at 1 MS/s; 1.6 MHz at 2 MS/s |
| Desktop LoRa/packet features | Disabled and hidden | Disabled and hidden |
| Main gain controls | LNA, VGA and RF amplifier | Tuner gain, manual or automatic; actual applied manual gain is recorded |
| Receive adapter | Repository-local static libhackrf 2024.02.1 and shared libusb 1.0.30 | Pinned minimal librtlsdr source linked to the same reviewed shared libusb 1.0.30 |
| CMake option | `OVMESH_ENABLE_HACKRF` | `OVMESH_ENABLE_RTLSDR` |
| Current hardware evidence | Local macOS reception and survey checks | Local macOS R820T reception, recording and waveform-observation checks |

These spans are application limits, not measured guarantees of a flat or interference-free passband. FFT edge rounding and the receiver-center guard leave explicitly unassessed frequencies. A 16 MHz HackRF survey remains a performance/passband target to qualify. RTL-SDR rates above 2 MS/s and wider swept surveys are not implemented. Stop/Resume preserves an SDR survey across supported setting changes; changing receiver type requires New; no single RTL survey observes the entire 902–928 MHz band.

RTL tuning is restricted by this application to 24–1766 MHz, including the survey edges and corrected center. Actual coverage also depends on the attached tuner. One tested R820T does not establish compatibility with every RTL model or tuner. Receiver Offset and serial selection reset when switching hardware types. See [RTL-SDR setup and troubleshooting](rtl-sdr.md) and the [user guide](../user-guide.md#configure-a-spectrum-receiver).

## RAK5146 USB/LBT

The initial adapter targets the **US915 USB/LBT RAK5146-126**, using one or two selected boards. Other RAK variants, SPI carriers and concentrator models are not validated by this support claim.

| Capability | Experimental RAK5146 adapter |
|---|---|
| Sampled scan range | Configurable range within 902–928 MHz; scan-filter edges must fit within the selected range |
| Scan method | Auxiliary SX1261 RSSI histogram: 33 counters, 2,000 samples per completed scan, nominal 234.3 kHz filter |
| Scan spacing | 25–1000 kHz; two boards split the assigned scan centers |
| Desktop packet reception | Disabled and hidden; existing profiles cannot enable it |
| Two-board use | Split the assigned sampled RF scan centers across two selected boards |
| Gain / power | Nominal vendor RSSI scale; no SDR LNA/VGA controls or transmitter-power measurement |
| Driver | Pinned minimal Semtech HAL, one isolated local receive-only worker per USB device |
| Build option | `OVMESH_ENABLE_RAK5146`; no packet forwarder, network service or firmware flasher |
| Validation | macOS application checks passed with one and two boards scanning while receiving configured packets; a visible two-board desktop check saved CRC-valid LongFast and LongTurbo receptions. Endurance and Linux hardware qualification remain pending |

This mode does not produce IQ, FFT bins or an SDR waterfall. RSSI sample exceedance is not continuous busy time, channel utilization or packet airtime. The scanner misses activity between visits and does not classify its energy by protocol. Retained developer packet reception is outside the current desktop scope. Neither sampled RSSI nor a scan-filter width identifies a modem preset or protocol.

The worker supports auxiliary scanning on one connection (its retained packet-polling path is disabled by the desktop), checks initialization/readback, and stops on transport or framing failure. The application must retain `ovmesh-rak-worker` beside its executable; macOS bundle and install rules include it. It never requests MCU bootloader mode or implements RF transmission. No carrier GPS or PPS integration is inferred from USB connectivity. See [RAK setup](rak5146.md), [concentrator measurements and reports](../design/concentrator-measurements.md) and [source provenance](../../third_party/sx1302_hal/PROVENANCE.md).

## Platform status

| Platform | Status |
|---|---|
| macOS / Apple Silicon | Earlier HackRF/RTL local USB checks and RAK one/two-board scan, packet, recording and export checks. A macOS 13.0+ arm64 app/DMG candidate has offline tests and packaged synthetic launches on macOS 26.3; the 0.4.1 app is Developer ID signed and notarized, and packaged HackRF reception has an operator-confirmed check. Minimum-OS, clean-machine and full packaged hardware qualification remain outstanding |
| Ubuntu 24.04 / amd64 and arm64 | Independent application builds, offline test checks and Xvfb GUI launches passed, including the RAK worker build. Both architectures passed package installation, synthetic CLI/demo and removal checks. USB permissions, GPS, physical-desktop and live receiver acceptance remain unqualified; other distributions are not implied |
| Windows / x64 | HackRF/RTL build preparation underway; no validated application or portable ZIP yet. Driver binding, GPS and live receiver acceptance remain qualification work. RAK worker is disabled; no Windows RAK support is claimed |

Build dependencies, package evidence and remaining gates are described in [deployment](deployment.md#platform-and-distribution-limits). These candidates are not published binary releases. RAK requires safe child-process descriptor closure on Linux (glibc 2.34+); unsupported builds disable the worker. A missing adapter at build time is different from a connected device being unavailable or busy. The application does not install system drivers or change USB permissions automatically.

The USB dependency helper targets native macOS/Linux arm64 and x86_64. The updated local USB build has macOS ARM64 Release-build and offline-test results in the [USB validation record](../security/usb-intake.md#validation-and-limits). Earlier macOS reception results above do not qualify hardware behavior after the dependency update. Windows dependency preparation, packaging and live USB acceptance remain separate work.

## Shared measurement and decoding limits

HackRF and RTL-SDR report uncalibrated received dBFS; RAK reports nominal vendor RSSI in dBm. Neither establishes transmitter watts. Gain, antenna, threshold, clipping and observation coverage affect comparisons. Spectrum coverage and LoRa discovery loss are separate measurements. Neither a moving waterfall, completed sweep nor a successful detection proves complete capture.

All desktop receiver choices are spectrum-only. LoRa waveform discovery, automatic/manual PHY decoding, RAK packet profiles and classification controls remain disabled even when old preferences enabled them. The code and earlier test evidence remain for future development; they are not supported desktop features. Broader model, platform, GPS-concurrency and endurance testing remain necessary. See [validation scope](../engineering/quality-and-validation.md) and [RF capability gaps](../engineering/rf-survey-gap-analysis.md).