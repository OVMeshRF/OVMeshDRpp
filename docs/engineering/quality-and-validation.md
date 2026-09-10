# Validation scope and qualification

Implementation, synthetic correctness and field qualification are separate. This page describes available software checks, known limitations and the evidence required for qualification. Run the applicable checks against each release candidate.

## Existing software checks

The [CMake test definitions](../../CMakeLists.txt) build native tests from [tests](../../tests/README.md). The following describes their intended coverage; run the configured suite and record its actual result for the revision being assessed.

| Area | Existing checks |
|---|---|
| Spectrum and occupancy | Analytic signal references, FFT normalization, known activity masks, simultaneous versus alternating bins, selected edges, gaps and observation denominators |
| Compact storage | Detailed/compact activity and GPS equivalence, power-block support, schema handling and saved query/report consistency |
| PHY and protocol | Symbol/framing checks, chunk boundaries, malformed input, explicit key behavior, schema presence/defaults and independent official-version fixtures |
| Waveform discovery | Analytic chirps, BW/SF inference, interference/adversarial cases, queue/worker behavior, persistence and processing-loss reporting |
| GPS and preferences | Checksums, valid/invalid/stale fixes, discovery selection, connection lifecycle, settings isolation and safe defaults |
| Reports and files | Selection consistency, privacy defaults, escaping, output bounds, source immutability, overwrite refusal and incomplete-output handling |
| Desktop | Session workflows, selectors, occupancy/geographic displays, timestamps, licenses and PNG capture |
| Dependencies | Offline vendored-file hashes, primitive crypto examples, compiled notice content and bounded generated schemas |

Fixtures contain public examples or constructed inputs, not operational captures. Sharing implementation ancestry limits independence: agreement between two derived decoders is weaker evidence than agreement with a separately generated reference.

## Known limitations

Successful software checks do not qualify the RF receiver, USB path or GPS for every field condition. Payload CRC failures, missed waveforms, discovery processing losses and receiver interruptions are known failure modes. Discovery has separate capacity counters because a working spectrum pipeline does not establish that all waveform work kept pace. Reliable automatic range-wide payload decoding remains incomplete.

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
