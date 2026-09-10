# Current implementation and limitations

Version 0.4.0 is experimental desktop survey software. It records frequency-resolved RF measurements over a supplied usable range and supports local analysis by frequency, time and receiver position. It is not a calibrated instrument, exhaustive mesh inventory or qualified channel-selection system.

## Available functionality

| Area | Implemented behavior | Boundary |
|---|---|---|
| Receiver | Direct receive-only HackRF adapter and deterministic synthetic source; center, signed Offset, sample rate, span and gain controls | RTL-SDR is not implemented; sustained operation and usable passband require setup-specific validation |
| Desktop | Live survey and Analyze workspaces; grouped Settings; fresh sessions; New, Open, Save, Save copy and spectrum/waterfall PNG capture | No automatic RF start or historical-session load on ordinary startup |
| Preferences | Non-secret receiver/source/display settings, recording folder and explicit opt-outs; preference format 5 reads formats 1–4 | Coordinates, messages, channel keys and historical results are not preference defaults |
| Measurement | Contiguous complete 4096-sample periodic-Hann FFTs, fine joint activity masks, powers, generic energy events and known gaps | Fixed saved threshold and uncalibrated dBFS; event counts are not packet counts |
| Live analysis | Full-range per-bin occupancy/power overview with actual frequency edges and observed/busy time | Available without decoded packets or recording; memory-only history cannot later be reconstructed |
| Saved analysis | Arbitrary frequency/time selection, optional receiver-area filtering, time/position plots and coverage accounting | Selected-bin time union differs from mean frequency-time occupancy; missing positions limit geographic results |
| Recording | Compact schema 6 or Detailed schema 5; typed waveform/discovery records, authorized decoded content and original receiver fixes | Plaintext SQLite; schemas 1–5 remain readable without migration, but old files lack measurements never recorded |
| Reports | Filtered CSV summaries, separate waveform/GPS/content reports, explicit detailed CSV/GeoJSON archives and narrative HTML findings | Privacy choices apply independently; HTML reports exclude message contents and default to excluding coordinates/free-form provenance |
| Session management | Durable save checkpoints, private new-file copies, deliberate historical Open and confirmed memory-only discard | A checkpoint during reception is not a finalized recording; older readers may not support current formats |
| GPS | Fixed receiver position, checksummed NMEA, OS metadata discovery and recognized/remembered selected-device connection | Connection/fix/error states are distinct; no surveyed accuracy, PPS timing or transmitter location |
| Waveform discovery | Experimental blind LoRa preamble detection across the supplied range for 125/250/500 kHz and SF7–12 | Inferred settings are waveform evidence; processing losses are recorded separately |
| Meshtastic | Up to four explicit frequency/BW/SF profiles; finite survey keyring; AES-CTR and bounded official-schema projections | No automatic dispatch from discovered waveforms, recipient-PKI decoding or MeshCore decoder |
| Presentation | Full UTC timestamps with timing provenance, explicit unknown/quality states and current session disk-size indicator | Host/sample-derived timing is not hardware timestamping; display refresh is not measurement completeness |

## Measurement interpretation

The [spectrum specification](../design/spectrum-measurement-record.md) defines normalization, time/frequency support, masks, center-guard comparisons and quality flags. Brief activity can occupy little average frequency-time resource while making an interval busy whenever any selected bin is active. A persistent receiver-center signal can dominate raw any-bin occupancy; excluding its guard is a separate diagnostic comparison that leaves those frequencies unassessed.

Compact recording preserves original fine activity, gaps and GPS references but shares powers over contiguous blocks of up to one second. Shorter time/area queries must retain the aggregation warning. Geographic queries use recorded receiver associations; missing fixes are excluded by an area filter and never replaced by a guessed location. Original coordinates are not smoothed or snapped.

Generic energy grouping reduces fragmented result lists but cannot establish packet boundaries or modem bandwidth. Experimental coherent waveform observations provide separate inferred BW/SF. The application does not subtract decoded packet counts from energy measurements to invent non-mesh airtime.

## Protocol coverage

The current PHY handles selected explicit-header LoRa profiles at SF7–12, BW125/250/500 kHz and CR4/5–4/8. Complete RF coverage is independent of decoder coverage. Empty reception lists can result from unsupported settings, weak-signal acquisition, synchronization/clock error, CRC failure, missing keys or unsupported content.

Meshtastic projections use Nanopb 0.4.9.2 and official v2.8.0 schemas, with independent v2.7.19/v2.8.0 compatibility fixtures. Text, selected position/user/telemetry/routing/traceroute fields and request/reply identifiers are supported within explicit bounds. Signature presence is reported without signature verification. Successful AES-CTR decryption and parsing do not authenticate the sender. See the [wire reference](../research/meshtastic-wire-baseline.md).

Automatic discovered-waveform payload dispatch remains absent. MeshCore, reliable LoRaWAN/other-device attribution, recipient private-key messages and exhaustive full-range mesh decoding are not implemented.

## Validation status

[Validation scope](quality-and-validation.md) describes existing test fixtures, known limitations and outstanding qualification. Software checks cover measurement unions, compact storage, GPS lifecycle, protocol fixtures, reports and desktop controls. Bounded HackRF/GPS observations demonstrate some successful recording and native LongFast content, alongside receiver interruptions, discovery losses and payload CRC failures. Those observations do not qualify every configuration or this snapshot independently.

Power/frequency calibration, receiver sensitivity, field false-alarm/detection rates, moving-GPS accuracy, long-duration mobile recording and Windows/Linux runtime acceptance remain outstanding. Received dBFS cannot identify an unknown transmitter's watts, hardware model or compliance. A sample survey cannot establish an interference-free channel.

## Deferred capabilities

There is no multi-session regional merge/comparison, offline basemap, calibrated receiver normalization, automated channel recommendation, plugin system, online map, network listener or automatic updater. Local system fonts are used without bundling them; platform-specific packaging and dependency closure remain release gates. See [capability gaps](rf-survey-gap-analysis.md), the [roadmap](../product/roadmap.md) and [build/package limits](../operations/deployment.md).
