# Current implementation and limitations

Version 0.4.0 is experimental desktop survey software. It records frequency-resolved RF measurements over a supplied usable range and supports local analysis by frequency, time and receiver position. It is not a calibrated instrument, exhaustive mesh inventory or qualified channel-selection system.

**Desktop scope:** spectrum surveying only. LoRa discovery, preset/key setup, packet reception and classification are disabled and hidden, overriding prior preferences and explicit desktop flags. The implementation remains for internal development. The native macOS rebuild, focused regression checks and ordinary fresh startup with previously enabled decoder preferences passed. Live reception has started; quality counters and longer endurance remain unassessed; earlier decoder tests are not a release qualification.

## Available functionality

| Area | Implemented behavior | Boundary |
|---|---|---|
| Receiver | Direct receive-only HackRF and RTL-SDR adapters plus deterministic synthetic source; center, signed Offset, sample rate, span and receiver-specific gains | RTL supports 1/2 MS/s and up to 1.6 MHz spectrum span; wider sweeping is absent. Sustained operation, model compatibility and usable passband require setup-specific validation |
| Concentrator | Experimental one/two-board RAK5146 USB/LBT adapter with a shared RSSI sweep range; desktop packet reception disabled | US915, macOS/Linux worker; no Windows worker, IQ waterfall, continuous occupancy or blind packet discovery. See [RAK setup](../operations/rak5146.md) |
| Desktop | Live survey and Analyze workspaces; grouped Settings; fresh sessions; New, Open, Save, Save copy and spectrum/waterfall PNG capture | Independent spectrum/waterfall resize handles; sidebar session/status controls; no automatic RF start or historical load |
| Preferences | Non-secret receiver/source/display settings, recording folder and explicit opt-outs; preference format 8 reads formats 1–7 | Coordinates, messages, private channel keys and historical results are not preference defaults; selected USB identities remain local |
| Measurement | Contiguous complete 4096-sample periodic-Hann FFTs, fine joint activity masks, powers, generic energy events and known gaps | Fixed saved threshold and uncalibrated dBFS; event counts are not packet counts |
| Live analysis | Full-range per-bin occupancy/power overview with actual frequency edges and observed/busy time | Available without decoded packets or recording; memory-only history cannot later be reconstructed |
| Saved analysis | Arbitrary frequency/time selection, optional receiver-area filtering, time/position plots and coverage accounting | Selected-bin time union differs from mean frequency-time occupancy; missing positions limit geographic results |
| Recording | Compact schema 6 or Detailed schema 5; RF activity/power records and original receiver fixes; new files include acquisition provenance tables | Plaintext SQLite; historical schemas 1–7 remain readable without migration, but old files lack measurements never recorded. Older readers may reject the acquisition extension |
| Concentrator recording | Schema 7 retains complete sampled RSSI histograms, GPS associations and per-board health | Separate sampled measurement semantics; no USB identities or unknown payloads in saved surveys |
| Reports | Filtered CSV summaries, receiver-GPS reports, explicit detailed CSV/GeoJSON archives and narrative HTML findings | Privacy choices apply independently; HTML reports exclude message contents and default to excluding coordinates/free-form provenance |
| Session management | Stop/Resume within the current survey, per-acquisition settings and pause gaps, durable save checkpoints, private new-file copies, read-only Open and confirmed memory-only discard | A checkpoint during reception is not a finalized recording; older readers may not support current formats |
| GPS | Fixed receiver position, checksummed NMEA, OS metadata discovery and recognized/remembered selected-device connection | Connection/fix/error states are distinct; no surveyed accuracy, PPS timing or transmitter location |
| Presentation | Full UTC timestamps with timing provenance, explicit unknown/quality states and current session disk-size indicator | Host/sample-derived timing is not hardware timestamping; display refresh is not measurement completeness |

## Measurement interpretation

The [spectrum specification](../design/spectrum-measurement-record.md) defines normalization, time/frequency support, masks, center-guard comparisons and quality flags. Brief activity can occupy little average frequency-time resource while making an interval busy whenever any selected bin is active. A persistent receiver-center signal can dominate raw any-bin occupancy; excluding its guard is a separate diagnostic comparison that leaves those frequencies unassessed.

Compact recording preserves original fine activity, gaps and GPS references but shares powers over contiguous blocks of up to one second. Shorter time/area queries must retain the aggregation warning. Geographic queries use recorded receiver associations; missing fixes are excluded by an area filter and never replaced by a guessed location. Original coordinates are not smoothed or snapped.

Generic energy grouping reduces fragmented result lists but cannot establish packet boundaries or modem bandwidth. The spectrum-only desktop does not infer LoRa BW/SF. The application does not subtract decoded packet counts from energy measurements to invent non-mesh airtime.

## Retained protocol development code

The retained PHY handles explicit-header LoRa at SF7–12, BW15.625/62.5/125/250/500 kHz and CR4/5–4/8 in internal diagnostics. The desktop does not start discovery or a decoder, install public keys, or expose manual/preset controls. This backend remains available for future engineering work, not as an alternate supported user feature. Reliable live range-wide protocol capture is unqualified.

Meshtastic uses Nanopb 0.4.9.2 and pinned official schemas for bounded envelope parsing. Inner application payloads are skipped. No text, node identities, packet IDs, locations, telemetry or routes are interpreted. Optional retained evidence is limited to envelope port and signature presence; likely Meshtastic is an unauthenticated inference and may be a false positive. See the [wire reference](../research/meshtastic-wire-baseline.md).

The catalog lists all 17 recognized Meshtastic presets, including the historical VeryLongSlow profile and seven 2.8 additions. LongSlow and VeryLongSlow carry deprecation notices; Tiny presets use actual 15.625 kHz modem bandwidth. The retained keyring model uses one public default across presets and explicit transient private keys; the desktop gate disables it. RAK packet support remains configured-profile-only in developer diagnostics and does not support 15.625/62.5 kHz modem widths.

Automatic candidate, frame, CRC and classification counters are distinct. Missing history, decoder capacity limits, timeouts, resets and dropped results remain visible limitations, not quiet time. MeshCore, reliable LoRaWAN/other-device attribution, recipient private-key messages and exhaustive full-range mesh decoding are not implemented.

## Validation status

[Validation scope](quality-and-validation.md) describes existing fixtures, known limitations and qualification still required. Measurement, GPS, PHY and discovery checks remain relevant to their tested revisions. Historical content-decoding results do not validate the present envelope-only classifier. Current local synthetic tests must establish content exclusion from protocol, storage, UI, exports and copy paths; they do not establish field classification rates.

Apple Silicon package preparation and independent Ubuntu 24.04 amd64/arm64 builds have offline test and synthetic GUI results. macOS signing/notarization, Linux package completion and Windows build preparation have separate gates; see the [platform validation details](../operations/deployment.md#platform-and-distribution-limits). These results do not qualify live USB/GPS behavior after dependency changes or replace clean-machine acceptance.

Power/frequency calibration, receiver sensitivity, field false-alarm/detection rates, moving-GPS accuracy, long-duration mobile recording, Windows runtime acceptance and Linux physical-desktop/hardware acceptance remain outstanding. Received dBFS cannot identify an unknown transmitter's watts, hardware model or compliance. A sample survey cannot establish an interference-free channel.

## Deferred capabilities

There is no multi-session regional merge/comparison, offline basemap, calibrated receiver normalization, automated channel recommendation, plugin system, online map, network listener or automatic updater. Local system fonts are used without bundling them; platform-specific packaging and dependency closure remain release gates. See [capability gaps](rf-survey-gap-analysis.md), the [roadmap](../product/roadmap.md) and [build/package limits](../operations/deployment.md).

## Receiver timing limitation

The current payload receiver consumes fixed-size symbol windows after initial synchronization. It has no continuous sample-clock tracking or fractional timing correction. Synthetic impairment diagnostics reproduce completed, valid-header frames that fail payload CRC with clock/timing offsets on both 250 and 500 kHz profiles. A failed integrity check occurs before key classification. This identifies a robustness gap; it does not prove the cause of any particular over-the-air failure. Remaining decoder work includes fractional timing/sample-clock compensation and wider independent validation of the bounded discovery-to-decoder handoff. Do not relax CRC validation to increase apparent success.
