# OVMeshDR++

**Community & support:** Join the [OVMesh Discord](https://discord.gg/kwKhFamfaU).

**Hardware support:** **HackRF One is the recommended hardware for wider RF surveys.** Its direct USB receive adapter supports 8, 10, 12, 16 or 20 MS/s, with a configurable spectrum span of up to **16 MHz at 20 MS/s**. The default is **10 MHz at 16 MS/s**; full-width performance still requires validation. RTL-SDR also has direct USB reception and supports 1–2 MS/s, up to **1.6 MHz** for spectrum surveying at 2 MS/s. Experimental **RAK5146 USB/LBT** support adds one or two concentrators for sampled RF scans on macOS and eligible Linux builds. RAK scans are sampled RSSI histograms, not an SDR waterfall or continuous band coverage. See [receiver and platform compatibility](docs/operations/hardware-compatibility.md), [RTL-SDR setup](docs/operations/rtl-sdr.md), and [concentrator measurements](docs/design/concentrator-measurements.md).

OVMeshDR++ is a local, receive-only spectrum survey application with an SDR++-inspired desktop interface. Observe a configurable frequency range, save measurements with optional receiver GPS, and explore activity by frequency, time and location. The application runs locally; it has no cloud service, automatic uploader or online map.

**Version 0.4.1 is an early experimental release with a spectrum-only desktop scope.** LoRa discovery, modem/preset identification, packet decoding and classification controls are disabled and hidden, including when older preferences requested them. The experimental code remains for future development; it is not a current desktop capability. Platform checks and remaining acceptance limits are documented with the packages. See [capabilities and limitations](docs/engineering/implementation-status.md).

## What you can do

- View the live spectrum and waterfall while recording a supplied usable range.
- Open a fresh session, save a copy, or deliberately open a previous survey.
- Record fine activity timing, coverage gaps, receiver settings and optional GPS associations in local SQLite sessions.
- Select arbitrary frequency intervals and compare busy time, observed duration, power and activity over time.
- Export compact frequency/time/location summaries, detailed archives, and a readable HTML analysis report with explicit privacy choices.
- Save a PNG of the visible spectrum/waterfall and view the current session's disk usage.
- Use one or two RAK5146 USB/LBT concentrators to collect sampled RSSI histograms, with separate recording and reporting semantics.

## Screenshots

These historical desktop examples use synthetic input and predate the spectrum-only interface. Any pictured LoRa observations, decoding or classification controls are unavailable in the current desktop. The images have not been regenerated and do not establish field performance.

![Synthetic live spectrum and waveform observations](docs/screenshots/live-survey.jpg)

![Frequency analysis of a synthetic saved survey](docs/screenshots/analyze-frequency.png)

### Sample analysis report

![Analysis report from a synthetic survey](docs/screenshots/analysis-report.png)

The report explains observation coverage, busy time, frequency activity and measurement limits for the selected scope. This example uses generated RF data and includes no actual receiver location. The [gallery](docs/screenshots/README.md) shows more screens and explains how to interpret them.

## Build and use

Start with the [build instructions](docs/operations/deployment.md) and [user guide](docs/user-guide.md). The synthetic receiver lets you explore the application without radio hardware. Ordinary startup opens an empty session with RF stopped; enabled recognized GPS setup may connect automatically. Saved receiver preferences persist, while old survey results and private keys are not automatically restored.

**Building on Linux?** Follow the [Linux setup guide](docs/operations/linux-build.md). It prepares the required OpenSSL and USB libraries inside the checkout without replacing system libraries or requiring a Python virtual environment.

The core uses C++20/CMake, with a Dear ImGui/GLFW/OpenGL desktop, SQLite, bounded Nanopb decoding, OpenSSL libcrypto, and direct HackRF or RTL-SDR/libusb reception. RAK5146 uses a minimal pinned Semtech HAL subset in a separate local receive-only process. Dependencies are pinned and reviewed; normal builds do not download them. The RAK worker requires macOS or Linux with safe child-process descriptor closure (glibc 2.34+); it is disabled on Windows.

**Downloads:** Get the experimental **0.4.1** packages from [GitHub Releases](https://github.com/OVMeshRF/OVMeshDRpp/releases): Apple Silicon macOS and Ubuntu 24.04 x64/ARM64. The Mac app uses Developer ID signing and Apple notarization; check the release notes for the final asset verification and checksums. Windows downloads are still in development. See [installation and platform limits](docs/operations/deployment.md#platform-and-distribution-limits).

HackRF and RTL-SDR builds use repository-local **shared libusb 1.0.30**; HackRF also uses **static libhackrf 2024.02.1** from the same prefix. On native macOS/Linux, explicitly prepare them with `python3 tools/bootstrap_usb.py --download`, or provide both pinned archives for an offline build. CMake rejects missing or mixed USB dependencies instead of falling back to host libraries. See [build setup](docs/operations/deployment.md#build) and [USB intake and validation limits](docs/security/usb-intake.md). This dependency update does not qualify Windows packaging or new hardware configurations.

## Understand the measurements

- **Occupancy** is above-threshold busy time divided by observed time over the same selected frequency/time scope. Unobserved time is not quiet time.
- **SDR power** is uncalibrated digital dBFS, not transmitter watts or antenna-port dBm. RAK reports uncalibrated vendor RSSI estimates in dBm; neither scale establishes transmitter power.
- **Energy-event width** is an above-threshold frequency envelope, not LoRa modem bandwidth, spreading factor or regulatory occupied bandwidth. Event counts are not packet counts.
- **Receiver GPS** locates the observation, not the transmitter. Fix quality and missing positions matter.
- A quiet-looking interval does not establish an interference-free channel or regulatory compliance.

The initial HackRF survey span is 10 MHz; selecting RTL-SDR sets 2 MS/s and a 1.6 MHz spectrum span. Remembered settings may differ. RAK scanning visits configured centers within 902–928 MHz sequentially; its sample-exceedance percentage is distinct from SDR busy time. Usable passband, sensitivity, mobile positioning, long-term capture reliability and protocol recall require further field validation. Read the [measurement reference](docs/design/spectrum-measurement-record.md), [concentrator measurement limits](docs/design/concentrator-measurements.md) and [RF capability gaps](docs/engineering/rf-survey-gap-analysis.md) before interpreting results.

## Privacy and security

New desktop recordings store RF measurements and optional receiver GPS; the disabled decoding paths produce no new packet classifications. Saved sessions still contain sensitive receiver locations or operator notes and are not encrypted at rest. Existing files remain unchanged, including any earlier diagnostic metadata; hiding a feature does not erase old recordings. Semantic message contents and sender/node identities are not interpreted or exported. See [data handling](docs/design/data-model-and-retention.md) and [security/privacy](docs/security/security-and-privacy.md).

Use [private security reporting](SECURITY.md) for suspected disclosure or vulnerabilities. Do not attach real recordings, channel keys, node databases or precise routes to public issues.

## Documentation and contributions

[Documentation](docs/README.md) · [Roadmap](docs/product/roadmap.md) · [Architecture](docs/design/architecture.md) · [Validation](docs/engineering/quality-and-validation.md) · [Support](SUPPORT.md) · [Contributing](CONTRIBUTING.md)

The combined application uses GNU GPL version 3; original project code is GPL-3.0-or-later and third-party grants remain intact. See [LICENSE](LICENSE), [NOTICE](NOTICE), and [third-party credits](THIRD_PARTY_NOTICES.md). SDR++ inspired the interface, and selected SDRangel logic is credited in the source. This independent project is not affiliated with SDR++, SDRangel, Meshtastic or MeshCore.
