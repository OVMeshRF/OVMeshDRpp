# Changelog

## Unreleased

- Prevent system OpenSSL pkg-config metadata from adding an unintended Zlib dependency to the reviewed local crypto build; check Perl's Time::Piece prerequisite before setup starts.

- Add explicit local OpenSSL setup for native Linux/macOS and actionable CMake checks for missing, mismatched or insufficiently hardened crypto dependencies. Normal builds remain offline.
- Add a direct RTL-SDR receiver choice, receiver-specific controls, saved setup and acquisition provenance. Initial RTL LoRa discovery uses 2 MS/s over a guarded 1.5 MHz span; spectrum-only reception also supports 1 MS/s.
- Include the pinned minimal librtlsdr source and allocation-failure checks; builds remain offline.

## 2026-09-10 — Initial experimental source snapshot (0.4.0)

- Receive-only HackRF spectrum surveys with a live spectrum/waterfall, measured coverage and frequency activity.
- Fresh-session workflow, GPS association, compact local recording, and explicit opening of saved sessions.
- Frequency/time/location analysis with summary CSVs, detailed exports and privacy-controlled HTML findings reports.
- Full observation timestamps, session-storage size, local waveform screenshots and grouped Settings.
- Experimental LoRa waveform discovery and explicitly configured Meshtastic decoding. Automatic discovery-to-decoder dispatch and reliable range-wide protocol identification remain incomplete.
- Build, user, support, security and licensing documentation, with synthetic application screenshots.

This is an experimental source snapshot, not a supported binary release. See the [implementation status](docs/engineering/implementation-status.md) and [RF capability gaps](docs/engineering/rf-survey-gap-analysis.md) for validation limits.
