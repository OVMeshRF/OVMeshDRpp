# Validation scope and qualification

Implementation, synthetic correctness and field qualification are separate. This page describes available software checks, known limitations and the evidence required for qualification. Run the applicable checks against each release candidate.

## Existing software checks

The [CMake test definitions](../../CMakeLists.txt) build native tests from [tests](../../tests/README.md). The following describes their intended coverage; run the configured suite and record its actual result for the revision being assessed.

Security regressions include a headless chart fixture with 100,000 overlapping coverage gaps, checking bounded drawing geometry and preservation of source records. The synthetic CLI survey checks terminal escape sequences, Unicode formatting controls and unchanged source database bytes. OpenSSL setup tests replace the caller's archive after verification and require extraction to use the verified private copy. These are targeted regression checks, not a comprehensive audit of upstream implementations or a clearance for distributable packages.

| Area | Existing checks |
|---|---|
| Spectrum and occupancy | Analytic signal references, FFT normalization, known activity masks, simultaneous versus alternating bins, selected edges, gaps and observation denominators |
| Compact storage | Detailed/compact activity and GPS equivalence, power-block support, schema handling and saved query/report consistency |
| PHY and protocol | Symbol/framing checks, chunk boundaries, malformed input, explicit key behavior, bounded envelope acceptance/rejection, payload skipping and independent official-version fixtures |
| Waveform discovery | Analytic chirps, BW/SF inference, interference/adversarial cases, queue/worker behavior, persistence and processing-loss reporting |
| GPS and preferences | Checksums, valid/invalid/stale fixes, discovery selection, connection lifecycle, settings isolation and safe defaults; optional GPS failure does not block RF or disable recording |
| Reports and files | Selection consistency, metadata-only legacy/new projections and Save-copy reconstruction, privacy defaults, escaping, output bounds, source immutability, overwrite refusal and incomplete-output handling |
| Desktop | Session workflows, selectors, occupancy/geographic displays, timestamps, licenses and PNG capture |
| Dependencies | Offline vendored-file hashes, primitive crypto examples, compiled notice content and bounded generated schemas |

Fixtures contain public examples or constructed inputs, not operational captures. Sharing implementation ancestry limits independence: agreement between two derived decoders is weaker evidence than agreement with a separately generated reference.

Desktop GPS-start fixtures substitute metadata inventory and receiver operations in the actual desktop startup path. They cover absent/ambiguous/duplicated/remembered-missing devices, metadata and serial failures, both SDR choices, preserved recording and GPS preferences, old-source invalidation, waiting/valid/stale connections, genuine RF failure and passive/demo/authorization guards. They do not enumerate or open USB devices. Separate GPS lifecycle tests cover invalidating serial fixes while preserving an explicit manual position; native Linux/Windows device and UI acceptance remains necessary.

For the optional-GPS startup change, the macOS Release desktop build completed and `desktop_setup_ui`, `ui_workflow`, `gps_status` and `gps_discovery` passed. Serial lifecycle checks used a test-owned pseudo-terminal, not a USB GPS. This validates the local software behavior, not Linux/Windows USB operation.

The engine integration fixtures use consumer-paced synthetic input: the generator waits when the bounded input queue is full, preserving the waveform and sample-based RF timestamps. Hardware callbacks and ordinary real-time demo pacing are unchanged. Both recording modes require successful PHY decoding and key-scoped envelope classification, zero sample loss, complete accepted FFT exposure, saved-data consistency and session lifecycle checks. Optional `OVMESH_ENGINE_THROUGHPUT_TESTS` retains the separate real-time 16 MS/s pass/fail checks. Passing an offline fixture does not establish receiver throughput.

Before ADR-0010, both consumer-paced recording modes passed on macOS in Release and in an unoptimized Debug build. The historical Debug checks each decoded the expected message with zero drops and approximately 5.50 seconds of measured RF input while processing load was about 5, demonstrating correctness when processing takes longer than RF time. This is historical software evidence, not current semantic-message support or native Linux/USB qualification.

The corresponding Release build with both USB backends and the desktop disabled passed all 35 configured native tests, including the two optional real-time engine checks. The real-time checks remain host-specific; these results do not establish the same throughput on another machine.

## Metadata-only boundary validation

The 2026-09-11 macOS Release build completed for the desktop, CLI and all native
test targets after [ADR-0010](../decisions/0010-metadata-only-surveys.md). All 50
configured CTest checks passed across the full run and a focused retry: the
initial run passed 49, and an obsolete discovery-export assertion was updated to
require removed semantic columns to be absent before that test passed. The
synthetic CLI diagnostics also passed 38 argument/privacy guards plus saved
analysis, report and source-preservation checks. No USB devices were opened.

Checks cover opaque payloads and explicit key restrictions, historical schema
1–7 metadata reads without semantic projection, original-file byte preservation,
receiver GPS and RF measurement equivalence, and fresh metadata-only Save-copy
reconstruction. Artificial content injected into live rows and freed database
pages was excluded from copies. UI checks cover metadata reception details,
report selection and rejection of removed content-export options. These tests
do not establish field classification accuracy or platform/package security.
The native dependencies and RF algorithms were not changed by this work.

## Session controls and acquisition validation

The waterfall viewport regression checks actual rendered signal bounds at UI scales 1 and 1.5: resizing preserves row position/thickness while clipping or revealing retained history. It also covers frozen/stopped views, missed display publications, bounded eviction, frequency/session resets and Retina screenshot crop bounds. The waveform, desktop-workflow and capture UI tests pass. This is display validation; rows are not a calibrated time axis and no live RF validation is implied.

The macOS build passed all 51 configured CTest checks for the session-controls change. Coverage includes Compact and Detailed engine operation, Stop/Resume with changed frequency grids and settings, retained GPS/history, saved-file reopening/copying and per-acquisition reports. Guarded occupancy tests exclude intervals containing only receiver-center bins from the comparison denominator. Actual ImGui mouse-event fixtures cover independent spectrum/waterfall dividers and board-selection popups.

After the final layout changes, all ten focused UI checks and the desktop workflow test passed again. The direct one-candidate RAK selection mouse test verifies explicit selection without USB/GPS access and rejects duplicate, multiple or already-assigned candidates. Synthetic native rendering also verifies the sidebar and analysis layout.

The hardware-start failure path before worker creation was reviewed in code but has not been fault-injection tested. These checks add no live USB, RF metrology or native Linux/Windows qualification.

## Known limitations

Successful software checks do not qualify the RF receiver, USB path or GPS for every field condition. Payload CRC failures, missed waveforms, discovery processing losses and receiver interruptions are known failure modes. Discovery has separate capacity counters because a working spectrum pipeline does not establish that all waveform work kept pace. Automatic waveform-to-PHY dispatch is implemented; reliable range-wide live decoding remains unqualified.

These limitations must not be reported as quiet spectrum, absent traffic or successful protocol classification. Performance and reliability claims require reproducible tests of the shipped revision and intended workload.

## Qualification work still required

| Area | Acceptance evidence needed |
|---|---|
| Metrology | Calibrated level/frequency sweeps across gains and span; passband, dynamic range, adjacent-signal rejection, analog overload and uncertainty budget |
| Detection and decoding | Independently labeled signals across strengths, offsets, BW/SF/coding rates and overlaps; detection/false-alarm rates and failure-stage accounting |
| Regional surveys | Repeat visits and routes, coverage by frequency/time/area, receiver setup comparability and representative observation periods |
| GPS/time | Stationary reference and mobile checks, stale/missing fixes, clock changes, acquisition association and explicit timestamp uncertainty |
| Endurance/recovery | Sustained USB/CPU/storage load, device removal, disk full, abrupt stop, incomplete sessions and bounded shutdown |
| Platform/package | Native Windows/Linux build/runtime tests, driver permissions, dependency closure, signing and clean-machine installation |
| Privacy/security | Persistence-path and exception review, malformed-file/input tests, fuzzing and reviewed diagnostics; OS swap/crash-dump limits remain separate |

Define the configuration, duration, expected result, thresholds and resource budget before a performance test. Report observed failures and untested areas alongside successes. Do not infer packet collisions or transmitter power from CRC failures or received level alone.

## Reproducing and reporting checks

Follow the [build guide](../operations/deployment.md), run the relevant configured CTest suite and the offline vendored-source check, and record the source revision, toolchain, dependencies, test configuration and actual outcomes. Include sanitized fixture provenance and scope; exclude device identifiers, precise locations, private payloads, credentials and operational captures from public evidence.

Hardware tests require permission from the relevant equipment operator and a defined receive-only collection scope. Synthetic tests must not silently open hardware. Ordinary desktop GPS auto-connection is a documented product behavior, so hardware-free tests use isolated passive/demo/test modes rather than ordinary startup.

A supported release requires evidence for each support claim, current privacy/dependency review and explicit remaining limits. No single passing suite establishes regulatory compliance, exhaustive detection or absence of security defects.

## RTL-SDR integration coverage

The RTL adapter has hardware-free checks for offset-binary IQ conversion, supported gain selection, explicit-access guards, bounded rate/span validation, reader cancellation/restart, and 125/250/500 kHz waveform discovery through the 2 MS/s path. Driver tests inject allocation failures and locked/unlocked tuner responses; sanitizer runs cover those failure paths. Preferences, desktop source/rate/gain controls, old-session compatibility, RTL recording metadata and CSV/GeoJSON/HTML provenance are tested. Builds with both hardware backends disabled remain supported.

A macOS R820T receiver has also delivered live samples and LoRa waveform observations to the desktop and local recording. This validates an initial integration, not all RTL tuner models, calibrated gain/passband, end-to-end packet decoding, sustained mobile operation, or Windows/Linux device behavior. Clipping and loss indicators remain meaningful limitations; receiver power levels are uncalibrated. Exact local hardware/session evidence is not part of the public source distribution.

## RAK5146 integration coverage

Hardware-free checks cover profile/range validation, bounded worker records, USB transaction framing, auxiliary scan command order, complete histogram reads, receive FIFO bounds, one/two-board preferences, desktop controls, schema 7 round trips and report privacy. Scanner results remain distinct from SDR FFT occupancy. A separate RAK-disabled build retained HackRF and RTL-SDR support and passed its concentrator compatibility test without opening hardware.

The macOS Release integration build passed all 51 configured CTest tests. Focused AddressSanitizer/UndefinedBehaviorSanitizer runs passed the worker, receive-buffer and scan-command checks, followed by the parent-process, concentrator and schema-7 storage checks. The synthetic process fixture verifies preservation of completed records before errors and during a multi-buffer STOP drain, missing-END handling and consumer failures. Four Python survey-audit tests also passed. These checks do not open USB devices or establish exhaustive security qualification.

Bounded macOS application checks demonstrated concurrent packet polling and sampled energy scanning. A 35-second one-board run saved 463 histograms (926,000 RSSI samples) and two CRC-valid configured LongFast receptions. A 60-second two-board run saved 1,753 histograms (3,506,000 samples) across 129 distinct scan centers, with four CRC-valid LongFast receptions and one CRC failure. A subsequent visible desktop run lasting about 118 seconds saved 3,661 histograms (7,322,000 samples), 12 CRC-valid LongFast receptions and two CRC-valid LongTurbo receptions, with no CRC failures in that run. The desktop stopped cleanly and left its results open. No channel keys or GPS were used for these checks; CRC validity does not establish authenticated mesh identity or successful content decoding.

Saved databases passed integrity checks, retained all 33 counters totaling 2,000 samples per scan, kept board scan intervals ordered and separated, and produced a frequency-summary CSV. No FFT coverage was fabricated. Development uncovered USB stalls on larger auxiliary requests; individually acknowledged tuning commands and bounded histogram chunks allowed combined operation. A board left unresponsive by an earlier failure required physical USB reconnection. Automatic recovery, long-duration stability, platform qualification and calibrated RF performance remain open work. Exact device identities and operational recordings are excluded from public documentation.

## Local OpenSSL setup validation

The explicit native Linux/macOS setup helper and configure checks were validated on macOS with the pinned OpenSSL 3.5.8 archive. An official HTTPS download matched the recorded size/SHA-256; an independent local source build completed, staged a fresh prefix, and passed the existing hardening and AES-128/256-CTR NIST checks. The application CLI compiled against that prefix; crypto intake, protocol and preference tests passed. Linked dependencies showed no shared OpenSSL library.

Seven bootstrap tests cover archive integrity, unsafe extraction entries, destination boundaries, existing-prefix preservation, HTTPS destinations and platform selection. Fourteen CMake regression cases cover missing/default/explicit prefixes, missing archives, wrong or malformed headers, absent hardening with tests disabled, mixed or stale paths, dynamic-library overrides and rechecking changed headers. These are software/build checks, not Linux receiver or desktop acceptance. The private helper probe explicitly uses Unix Makefiles so a multi-configuration generator preference cannot change the expected test-executable location. Linux runtime and hardware validation remain pending.

The follow-up dependency-isolation fix reproduced the reported `ZLIB::ZLIB` generation error using synthetic system `openssl.pc` metadata advertising Zlib while Zlib discovery was disabled. After the fix, all 24 setup tests passed on macOS: eight bootstrap cases and sixteen CMake cases, including the generated bootstrap probe compiling and running the AES vectors under that same metadata. The fixture also verifies that pkg-config remains usable independently. The added Perl-module check fails before download/build when `Time::Piece` is unavailable. The application CLI rebuilt successfully; crypto intake, protocol and preference tests passed (3/3). This reproduces and tests the build-system failure without claiming native Linux runtime validation.

## Complete preset and automatic dispatch checks

The catalog is cross-checked against the vendored 2.7.19 and 2.8.0 enum definitions and pinned official RF parameter switches. All 17 parameter bundles, including historical VeryLongSlow, have clean synthetic PHY checks. Automatic discovery/replay tests generate all 17 at distinct arbitrary frequency offsets and sample phases, plus a CR4/7 case. They require exact CRC-valid bytes without installing fixed decoder frequencies. Separate checks cover asynchronous channelization, timestamp mapping, bad-CRC byte erasure, gaps, capacity bounds, and manual-profile exclusions. See [catalog provenance](../research/meshtastic-presets.md).

Those fixtures share the project's modulation/PHY implementation and cannot establish interoperability, sensitivity or exhaustiveness. Expanded narrow-band hypotheses require more CPU/memory. No new live RF result, sustained maximum-span capacity, Linux/Windows qualification or package acceptance follows from these offline tests.

For this change, the macOS Release desktop/CLI build completed and all 54 configured native checks passed across the full run and focused retries. The initial run exposed three legacy-fixture builders retaining the new extension and a UI fixture using the wrong ImGui coordinate origin; the fixtures were corrected without relaxing production schema validation or mouse assertions. Mixed on/off/on decoding preserves each acquisition in reopened/copied sessions, CSV/GeoJSON and report scopes. Public-key fixtures cover every catalog name plus a custom name.

Four standalone AddressSanitizer/UndefinedBehaviorSanitizer checks also passed: automatic discovery/decoding for all 17 presets plus CR4/7, expanded chirp screening against 277,644 reference statistics, repetition windows and FFT. No sanitizer diagnostics were reported. Those checks used existing local sources only, with no USB or external dependency acquisition.

## Discovery throughput follow-up

The expanded detector could overload even while the independent spectrum pipeline continued showing RF activity. Queue gaps reset preamble tracking; changing channel keys cannot repair that missing waveform continuity. The follow-up reduces repeated work in confidence calculation, ring resets and FFT peak extraction, and shares eligible subband jobs among the same three workers. It preserves all 28 BW/SF hypotheses, detection thresholds, per-subband ordering and the 48-job storage bound.

The macOS Release build passed all 55 configured native tests. This includes exact automatic PHY results for all 17 presets, interference/strong-signal cases, payload transitions, discontinuity reacquisition and skewed shared-worker scheduling. The compacted chirp screen matched 331,724 direct-reference statistics with no pass/fail decision differences.

The separate `discovery-worker-capacity` diagnostic ran with a 12.8 MHz survey span, 16 MS/s input, spectrum processing and automatic PHY dispatch. Its complete-packet scenario generates 250 kHz/SF11/CR4/5 and 500 kHz/SF11/CR4/8 bursts at arbitrary frequency offsets, without keys, hardware or recorded samples. Both the three-second and thirty-second runs accepted every input sample and produced every expected exact CRC-valid frame:

| Duration | Accepted samples | Rejected samples / resets | Exact frames | Final processing lag | Drain after source stopped |
|---|---:|---:|---:|---:|---:|
| 3 s | 48,000,000 | 0 / 0 | 6 / 6 | 9.9 ms | 7.5 ms |
| 30 s | 480,000,000 | 0 / 0 | 60 / 60 | 9.1 ms | 7.3 ms |

The thirty-second run consumed approximately 100.6 CPU seconds, equivalent to 3.35 fully occupied cores averaged across its elapsed time. This is measured process work on one development Mac, not a minimum CPU specification. Backlog did not grow in this fixture. The old desktop app and other build/test jobs were stopped for timing. These results do not qualify a regular MacBook, USB reception, live GUI/database overhead, weaker/colliding signals or every possible combination of simultaneous emitters. A narrower range changes coverage and must remain an explicit operator choice; loss counters must not be hidden or interpreted as quiet spectrum.

Final ThreadSanitizer checks passed for shared-worker ordering, queue saturation, resets, draining and exception handling. AddressSanitizer/UndefinedBehaviorSanitizer checks passed for all 17 automatic preset decodes plus CR4/7, bounded reset/lazy-confidence handling, and the chirp-screen reference comparison. All ran with halt-on-error enabled and reported no sanitizer findings. They do not establish absence of all concurrency or memory defects.

## Spectrum-only desktop scope

The desktop is restricted to RF spectrum measurement and sampled RAK scans, with LoRa discovery, decoder/profile/key controls and packet classifications disabled even for remembered configurations. Existing code and earlier offline tests are retained for future development. The macOS desktop rebuild and the focused `preferences`, `desktop_setup_ui`, `ui_workflow`, `report_ui` and `discovery_worker` checks passed. Ordinary startup was visually checked with older enabled preferences: it opened a fresh stopped spectrum-only survey with automatic recording armed and only Energy details below the waterfall. No release-readiness or cross-platform hardware claim follows from these checks.

The startup check also confirmed the remembered HackRF range, gains and Offset were preserved. Saved discovery, automatic-decode and public-key switches were disabled and spectrum-only was enabled. The report fixture was updated for the revised scope wording and passed on rerun. The operator subsequently started live reception: the waterfall and energy-group table visibly updated. That observation does not assess quality counters or endurance.

A recent approximately 51.3-second spectrum-only live check, preceding this desktop scope change, reported zero application input drops; upstream loss remained unknown. In a separate 120-second live check with LoRa discovery enabled, approximately 31% of discovery input was rejected despite the earlier passing synthetic capacity fixture. These are different processing paths and runs: the spectrum result does not validate protocol recall or the newly built desktop, and the live discovery loss cannot be hidden or interpreted as quiet RF. Exact devices, private filenames, locations and operational captures are excluded here.

The acceptance scope includes fresh ordinary startup; ignored old decoder preferences and explicit desktop decoder flags; absent live/saved waveform and classification selectors; RAK scan-only starts; preserved Stop/Resume, recording, GPS, spectrum/energy views and frequency/time/location reports; and unchanged original historical files. The focused checks and startup observation above cover local software behavior; live receiver validation, longer endurance on ordinary laptops, sustained mobile/USB/GPS behavior, calibration and platform packaging remain separate outstanding checks. Generic energy events remain envelopes, not packets, LoRa modem bandwidths or spreading-factor detections.
