# Receiver and platform compatibility

OVMeshDR++ provides direct, receive-only USB adapters for **HackRF One and RTL-SDR**. Select the receiver in the main dropdown while stopped. Both use the same spectrum/waterfall, session recording, analysis and export workflows; optional GPS is a separate input. This is experimental source support, with the validation limits below.

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

## Platform status

| Platform | Status |
|---|---|
| macOS / Apple Silicon | Development builds and local USB reception checks for both receivers; no signed/notarized redistributable package |
| Linux | Source targets the platform; compilation, USB permissions, GPS and live receiver acceptance still need validation |
| Windows | Source targets the platform; compilation, USB driver binding, GPS and live receiver acceptance still need validation |

Build dependencies and native USB requirements are described in [deployment](deployment.md). A missing adapter at build time is different from a connected device being unavailable or busy. The application does not install system drivers or change USB permissions automatically.

## Shared measurement and decoding limits

Both receivers report uncalibrated received dBFS, not transmitter watts. Gain, antenna, threshold, clipping and observation coverage affect comparisons. Spectrum coverage and LoRa discovery loss are separate measurements. Neither a moving waterfall nor a successful detection proves complete capture.

The RTL adapter does not change protocol maturity: LoRa waveform discovery is experimental, automatic discovered-waveform dispatch to payload decoding remains unfinished, selected-profile Meshtastic decoding remains experimental, and MeshCore decoding is not implemented. Broader model, platform, GPS-concurrency and endurance testing remain necessary. See [validation scope](../engineering/quality-and-validation.md) and [RF capability gaps](../engineering/rf-survey-gap-analysis.md).
