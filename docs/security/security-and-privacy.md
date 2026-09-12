# Security and privacy design

Status: security design with implemented Compact schema-6, Detailed schema-5 and concentrator schema-7 persistence controls and the version-0.4.0 desktop workflow. Spectrum metrology and retained-data boundaries are unchanged by the layout. Current precautions and residual risks are in [dependency inventory](../engineering/dependencies-and-licensing.md), [implementation status](../engineering/implementation-status.md) and the [UI validation record](../engineering/quality-and-validation.md). No complete security certification or exhaustive live-detection claim is made.

## Assets and boundaries

Protect configured channel secrets, receiver GPS tracks, time/frequency activity records, private survey notes, report integrity and the host. RF/GPS input, operator strings, saved databases and third-party dependencies remain untrusted. Semantic message contents and on-air sender/node/packet identifiers are excluded from new records, display, search, exports and copy reconstruction. Historical source files may still contain them outside this application's views.

Primary boundaries are receiver-to-parser, parser-to-key handling, decoder-to-record writer, local records-to-export, and future receiver-host-to-remote-client. UI and diagnostic paths must not bypass these boundaries.

The current [spectrum milestone](../engineering/implementation-status.md) measures the supplied range independently of protocol or keys and supports arbitrary frequency/bin, time and GPS-region analysis. It adds no channel recommendation engine, transmitter-authentication claim or new decoder capability. Definitions and uncertainty limits are in the [spectrum measurement record](../design/spectrum-measurement-record.md). Optional coherent LoRa waveform discovery adds inferred bandwidth/SF evidence without decoding payloads or identifying Meshtastic/MeshCore. Its bounded work and loss counters are separate from spectrum coverage; see [experimental validation and capacity limits](../engineering/quality-and-validation.md).

## Desktop preferences and GPS discovery

Recording, GPS connection, waveform discovery and key-scoped classification are enabled or armed by default, while preserving explicit opt-outs. Ordinary startup is a fresh empty workspace and keeps RF stopped. For a hardware-oriented profile, GPS auto-connect is limited to the enabled uniquely recognized or remembered selected device identified through OS metadata. Missing or ambiguous selection requires operator choice. Generic serial adapters and Meshtastic radios must not be automatically probed for NMEA. Metadata/identifiers remain bounded untrusted strings, never shell commands or ImGui format strings.

Preference format 8 stores GPS/recording switches and folder, GPS identity/baud, source, center/rate/span, HackRF LNA/VGA/amplifier, RTL tuner gain and automatic gain mode, selected RAK board identities and non-secret receive/scan profiles, signed Offset, Compact/Detailed mode, discovery/classification/Spectrum-only choices, the public-default-key switch, typography and position-display mode. Offset is bounded to ±100,000 Hz, defaults to zero and is not a per-device calibration record. Ordinary desktop defaults enable the visible Meshtastic public-key option; private keys must be explicitly supplied and remain in memory. This does not authorize unrelated channels or implement full-range payload dispatch. Spectrum only suppresses discovery and payload processing without erasing setup. Private keys, precise coordinates, secret profile material, decoded content and export privacy choices remain excluded. The public-key choice is stored only as a boolean. Parsing is bounded to 32 KiB and rejects malformed/unknown layouts. Formats 1–7 remain readable; present opt-outs survive migration to format 8, and absent fields use their documented defaults. Private temporary creation and atomic replacement preserve settings. POSIX app-owned directories/files use 0700/0600; the Windows owner-only ACL implementation still needs native validation. Permissions/hex encoding are not encryption, and folder/device identifiers remain identifying local metadata.

Invalid preferences are left untouched. Missing/unsafe destinations block requested recording instead of falling back to memory-only operation. New recordings use separate filenames and refuse replacement; historical results are never restored implicitly. Passive, demo, historical, CLI and managed-test paths do not load ordinary preferences or automatically connect GPS. Tests use explicit isolated repository-local settings; explicit CLI GPS access remains a separate operator-authorized action. Current workflow coverage is recorded in [UI validation](../engineering/quality-and-validation.md); a regression pass is not a general hardware or platform-security guarantee.

## Concentrator worker boundary

The optional RAK adapter uses a pinned minimal Semtech HAL in one local worker process per explicitly selected board. The parent resolves the worker beside its own executable, passes bounded arguments without a shell, restricts inherited descriptors/environment and validates every IPC record. The worker checks USB framing and response lengths, applies transfer deadlines, disables transmit/reset/bootloader entry points and suppresses upstream diagnostics. Startup does not probe unrelated serial ports. Separate processes isolate HAL global state; they are not OS security sandboxes.

CRC-valid frame bytes cross local IPC only for the configured receive profiles and remain transient. The parent applies the explicit survey keyring and bounded envelope parser before retaining minimal classification evidence. CRC failures never carry payload bytes. Temporary packet buffers are wiped, but swap and crash-dump limitations still apply. Schema 7 stores complete fixed-size RSSI histograms, configured receiver metadata, health and optional GPS associations; USB paths/identifiers are omitted. Unknown payloads have no database or export field. Source and local hardening changes are documented in [HAL provenance](../../third_party/sx1302_hal/PROVENANCE.md).

## Implemented spectrum persistence controls

- Schema 5 retains the phase-free power tiles, joint activity masks, generic energy-event envelopes, known gap intervals and method/provenance metadata introduced in schema 4. Its binary fields are bounded encodings of amplitude statistics and threshold bits; they cannot serve as a raw-IQ, ciphertext or undecoded-payload fallback.
- Tile readers enforce finite numeric bounds, matching session frequency grids, sample/time intervals, at most 4096 bins and 128 frames, exact power lengths, bounded RLE expansion and zero padding bits. Unknown or executable database schema objects are rejected; extensions and writes are disabled on read-only connections. Schemas 1–4 remain read-only without migration or invented waveform evidence; schemas 1–3 still lack the later detailed spectrum measurements.
- Schema 5 adds only typed waveform evidence, separate discovery status/subband counters and discovery-gap metadata. Exact discovery-table definitions, finite values, bounded positive match fractions, supported BW/SF values, at most 32 subbands and signed-SQLite-safe counters are enforced. Recent readback is capped at 256 waveform rows; full queries/exports validate older rows as they stream them. No IQ, ciphertext, undecoded payload or high-resolution symbol trace is stored by this path.
- A waveform's receiver fix is associated with delimiter acquisition time, or remains absent. Inferred bandwidth/SF and preamble evidence do not establish a packet, mesh protocol, transmitter identity, authentication or packet airtime. Rejected/abandoned discovery work and overflows remain explicit even when spectrum acquisition succeeds. The [paced comparison](../engineering/quality-and-validation.md) passed its narrow-span synthetic case and recorded wider-span overload; it is not an exhaustive-detection or field-validation guarantee.
- New local session/export files use private creation and refuse overwriting existing files or following supported path aliases. Mounted/network paths are rejected. File permissions do not provide encryption, erase backups or protect against a privileged host process.
- Prepared tile inserts and batched WAL transactions support recording without a full sync for every tile. Live analysis reads committed data through a separate read-only connection. The most recent uncommitted batch can be lost on a crash; journals and database companions remain sensitive survey data.
- Query display is bounded independently of complete-query totals. Invalid query bounds, malformed encoded measurements and unsupported database layouts fail explicitly. Known gaps, missing GPS and unknown upstream loss cannot be reported as quiet observation time. Resource limits and synthetic cases are not proof against every malicious input or overload condition.
- CSV contains no semantic message fields. Receiver coordinates and free-form provenance are excluded by default, with separate opt-ins. RF method/gain metadata and approved spectral measurements remain available. CSV formula neutralization applies to textual fields. GeoJSON requires receiver-coordinate opt-in and rounds coordinates to the selected precision; RF features describe receiver observation endpoints, never transmitter locations. Waveform features use the acquisition-time receiver association; unlocated waveforms and discovery coverage records retain null geometry. Exported waveform evidence has no packet-airtime or payload fallback.

Receiver GPS and private descriptions can reveal sensitive routes or operations even when no packet is decoded. Redacting receiver coordinates does not remove location information deliberately included in opted-in operator notes. Retained phase-free measurements still disclose local activity patterns and timing and require the same operational-data handling as other survey results. See the [data model and retention policy](../design/data-model-and-retention.md).

## Compact recording and summary reports

Schema 6 reuses the same allowlisted measurement and classification boundary. Power arrays have bounded dimensions and up-to-one-second support; full FFT activity and GPS references are validated. Stable GPS integer IDs preserve reference identity during SQLite maintenance. Missing references, inconsistent bounds, unsupported encoding/schema objects and malformed masks fail explicitly. No dependency or opaque payload field is added.

Filtered reports read one database snapshot and write a new private local file exclusively. On a failed report, cleanup targets only that newly created file. Text uses CSV formula neutralization; numeric fields are validated finite. Reports have explicit row/accumulator limits and never silently truncate. Receiver coordinates and free-form provenance remain independent opt-ins; GPS and geographic reports require position inclusion. Grid cell IDs reveal location even when displayed coordinates are rounded. Legacy detailed archives remain potentially large and sensitive. Existing databases remain unchanged; compact data requires a schema-6 reader.

## Session actions, images and fonts

**Save session** confirms the recording worker's committed SQLite checkpoint. New session files carry the validated `metadata-only-v1` policy marker. **Save a copy** reconstructs a pinned, identity-checked snapshot into a fresh private database using allowed records; it never copies database pages or free-page remnants. Reserved historical semantic fields remain NULL and route tables empty. Unmarked older recordings are refused before creating a destination. Their original files remain unchanged and can still contain historical private contents, although application result objects and RF metadata exports exclude those fields. Reading allowed columns does not guarantee that SQLite or the operating system never caches a source page containing old content. New-policy copies include original receiver positions and notes and are not privacy-filtered reports. Active copies remain marked incomplete/checkpoint. New requires consent to discard memory-only results. Files are not encrypted at rest.

Saved analysis and copy use independent read-only connections without holding the engine lifecycle lock throughout lengthy work. UI results are bound to their source session, and stale results must not be applied after New/Open. These controls preserve scope and responsiveness; they do not justify claiming every hostile database or resource-exhaustion case is handled.

The busy-time chart combines overlapping coverage gaps into bounded display columns before drawing their shading. Drawing memory does not multiply with duplicate gaps. Original gap records, exact time bounds and query results remain unchanged; only their visual union is rounded outward to display resolution. CLI analysis, device labels and error messages escape terminal control bytes at the output boundary, preserving the original stored text.

**Capture PNG** saves only the approved visible spectrum/waterfall crop and its axes/time/nominal receiver context, excluding GPS, private paths and decoded-message panels. It does not retain IQ or enable replay. Capture reads pixels before the file chooser covers the plot; overlay/crop guards and bounded dimensions protect that path. The owned uncompressed PNG writer uses private exclusive creation, with no new image package. A spectrum image still reveals frequency/time activity and requires operator review before sharing.

System fonts are loaded from local OS font locations with the existing font parser and built-in fallback. They are not downloaded, copied into the repository or bundled into the app by this change; applicable OS/font terms still apply. No new dependency, network listener, upload service or updater is introduced. [Dependency and licensing details](../engineering/dependencies-and-licensing.md) retain the release obligations.

## Threats and proposed controls

| Threat or failure | Proposed control | Required evidence |
|---|---|---|
| Accidental storage of IQ/ciphertext/undecoded bytes | Central allowlisted writer; no raw recorder/exporter; reviewed log/cache/dump paths. | Static data-flow review and controlled retention canary checks. |
| Key or private content leaks in diagnostics/export | Separate secret store, redacted diagnostics, local export preview, explicit field selection. | Sanitized-output review, including exceptional paths. |
| Wrong-key or malformed data treated as protocol evidence | Bounded envelope parsing, explicit unauthenticated/false-positive caveats and ambiguity rejection. | Wrong-key, random input, truncated frame, and schema-edge cases. |
| Claimed sender treated as authenticated identity | Separate decode, CRC, integrity, and identity status. | Protocol-specific tests and UI review. |
| Malicious RF/GPS/profile input crashes or exhausts the host | Length limits, bounded queues, time/resource budgets, reviewed parsers. | Robustness tests, fuzzing plan, and overload behavior. |
| Operator notes or imported labels become executable UI/export content | Render as inert text; sanitize format-specific exports, including spreadsheet formula prefixes. | Hostile-content rendering/export tests. |
| Precise receiver routes or positions are shared accidentally | Receiver-only position fields, location-precision controls, export preview. | Privacy walkthrough with synthetic locations. |
| Dependency introduces hidden persistence or outbound access | Version pinning, source/license review, capability minimization, SBOM at release. | Dependency inventory and approved runtime observation. |
| Vendored/pinned source misses security fixes | Source lineage, update owner, advisory/fix tracking, reviewed project updates. | Update records and affected regression/retention validation. |
| Live receiver access or RF transmission without approval | Exact-scope authorization; receive-only adapter and distribution profile. | Hardware-access review and approved validation logs. |
| Future remote client exposes content or control | Separate future threat review, authenticated access, explicit permissions, protected transport. | Required before any remote service is enabled. |

## Authorization policy

Ordinary desktop startup visibly enables the published Meshtastic public channel key through a separate survey-wide switch. Operators can disable it persistently. Only that published default is built in; private keys require explicit entry and remain transient. An empty protocol keyring, CLI, managed test or passive demo has no implicit fallback. Possession of a key does not authenticate a sender. Metadata-only retention remains unchanged.

Protocol recognition can inspect necessary public framing transiently. It does not authorize decrypting unrelated content or retaining packet bytes. New metadata fields need a reporting purpose and privacy review.

The optional batch runner opens only explicitly supplied radio/GPS paths and enforces a fixed three- or six-request plan with bounded reception, spacing, health checks and cleanup. Private per-step audit files use exclusive creation, reject symlink paths and persist a bounded metadata-only intent before any packet UART write; a failed intent write blocks that packet write. A second record marks a returned UART write. Interrupted or missing output remains an unknown outcome where the available evidence cannot establish completion. These records do not prove RF delivery and never contain payloads, keys or full device configuration.

## Retention and host limitations

The [Meshtastic wire-format baseline](../research/meshtastic-wire-baseline.md) identifies upstream behaviors requiring removal, adaptation, or validation before adoption. Review the whole shipped configuration, including core entry points and library diagnostics, rather than relying on hidden UI controls or a reduced plugin list. Explicit key scope and approved-record writing remain project-owned policy.

Review the complete storage surface: session database and journals, debug logs, framework logs, OS crash reports, swap/hibernation, temporary files, caches, support bundles, backups, and exports. Document residual OS limitations rather than claiming that memory-only buffering guarantees no persistence anywhere.

The retention requirement does not authorize altering global OS configuration. Any hardening that changes system settings requires a concrete proposal and authorization.

Local survey encryption, secret-store fallback, backup encryption, unlock behavior, and deletion semantics are undecided. Do not describe them as existing controls.

## Release security readiness

### Local narrative analysis export

The HTML analysis report uses a consistent read-only source snapshot and the existing exclusive private-file writer. It contains approved measurement summaries and counts, not keys, sender identifiers or message contents. GPS coordinates and free-form titles/descriptions/notes are separate opt-ins. It does not include the source file path. Every untrusted text field is HTML-escaped; inline styling is fixed application text, and a restrictive Content Security Policy blocks scripts, external resources, forms and base-URL changes. Generation makes no network request. Output is capped at 16 MiB, aggregation uses existing bounds, and failed writes remove partial output. These controls do not encrypt a report or make included coordinates/notes anonymous. A user may explicitly share or print it outside the application.

Before release: assign a security owner and private reporting route; review supported dependencies and versions; complete relevant parser/retention tests; document known limitations; define incident/advisory handling. Signing, notarization, and software composition evidence belong to [release engineering](../engineering/development-and-releases.md).

See [SECURITY.md](../../SECURITY.md) for current reporting and [the risk register](../governance/project-governance.md) for open risks.

## Resuming surveys

New recordings include a validated acquisition-segment extension. Start/Stop preserves the current recording while each resume records its frequency range, sample rate, gains, Offset, threshold and permitted decoder settings. Pauses remain explicit unobserved intervals. The extension contains no USB paths, serial identifiers or keys. Existing files are not migrated or modified by read-only Open. Save a copy retains the extension and metadata-only policy. Use this application version to read segmented recordings; older readers are not qualified for them.

HTML report Preview uses the same selection and GPS/notes opt-ins as export. It writes an exclusive, private local file in the default Surveys directory, retained until the user deletes it. Successful HTML generation invokes the OS default-browser handler for the checked absolute local file, without shell interpolation, a URL or a network service. An opening failure preserves the file. CLI exports do not open a browser.
