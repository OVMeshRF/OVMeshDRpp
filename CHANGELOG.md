# Changelog

## 0.4.2

- Add an active-frequency range summary and hide measured zero-activity rows by default, with an option to include them. Preserve unobserved rows and all recorded measurements.
- Add Export to PDF in HTML previews using the browser save dialog; automatically expand report sections for print/PDF.
- Rank busiest time groups outside the receiver-center guard and clarify missing GPS and unavailable protocol evidence.

## 0.4.1

- Focus the desktop on spectrum surveying. Experimental LoRa discovery, packet decoding and classification controls are disabled and hidden while development continues. Frequency, time and location measurements remain available; energy-event counts are not packet counts.
- Preserve measurements and recordings across Stop/Resume. Use New for a separate survey, Open for saved history, and Save for a durable checkpoint.
- Consolidate session controls and status in the sidebar. Resize the spectrum and waterfall independently without stretching waterfall history.
- Make brief activity visible in HTML reports using the desktop's labeled low-activity scale. Add local report Preview and automatic opening after generation.
- Improve RAK reports with per-board sampled-activity charts, actual filter bounds and scan/sample counts. These measurements remain distinct from continuous SDR occupancy.
- Improve optional GPS discovery, receiver selection, report diagnostics and Linux build prerequisites.
- Update the reviewed shared libusb dependency to 1.0.30 and harden supplied-archive handling, terminal output and saved-coverage rendering.
- Add repeatable Apple Silicon macOS and Ubuntu 24.04 amd64/arm64 packaging, matching source materials, dependency inventories and isolated package checks. Windows packages remain in development.

This is an early experimental release. Container and synthetic checks do not establish Linux USB/GPS operation, calibrated RF measurements or field reliability. See [platform status](docs/operations/deployment.md#platform-and-distribution-limits) and [measurement limits](docs/engineering/rf-survey-gap-analysis.md).

## 0.4.0 — Initial experimental source release

- Local receive-only RF surveys with HackRF and RTL-SDR, spectrum/waterfall display, GPS association and saved sessions.
- Frequency/time/location analysis, summary exports, HTML reports and waveform screenshots.
- Experimental one/two-board RAK5146 USB/LBT support with separately reported sampled RSSI scans.
- Build, user, support, security and licensing documentation.
- Early LoRa detection/classification work; this is disabled in the 0.4.1 desktop.
