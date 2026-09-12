# Test-data privacy

The tracked tests contain invented identities, sample coordinates, public protocol examples and deterministic cryptographic vectors. They are test inputs, not live GPS logs, channel secrets or application defaults. Generated files belong under ignored `build/` paths.

- `test_gps.cpp` labels the published RMC/GGA checksum examples and GPSD GN-talker example in `golden()`. Other sentences are constructed parser cases; no USB capture is loaded by these tests.
- Geographic/storage/report tests explicitly construct positions, including zeros, near-equator decimal values, polar limits and wraparound cases. Invalid coordinates deliberately test rejection. They do not establish an operator location.
- [Protocol fixtures](fixtures/README.md) are generated from official versioned schemas with invented values. Known cryptographic vectors and explicitly installed test keys must remain distinguishable from real operational keys. Runtime key normalization may recognize an explicitly supplied public-key shorthand; that is not permission to preload a key.
- RAK metadata-discovery and desktop tests use invented USB identities and port paths with fake receiver interfaces. They check explicit selection, missing/duplicate rejection, preference migration and sampled-RSSI UI semantics without opening USB/GPS or enumerating real devices. Stored histogram/report tests use constructed counts and coordinates rather than captured traffic.
- [Documentation screenshots](../docs/screenshots/README.md) distinguish synthetic interface examples from reviewed measurement-only report excerpts.

Do not replace these inputs with private captures to reproduce a bug. Prefer a minimal synthetic fixture, document its provenance and review every serialized field. Coordinates and secrets can be encoded in protobuf bytes, JSON, filenames, screenshots or Git history, so a text search alone is insufficient.

## Engine correctness and performance

`engine` and `engine_detailed` run consumer-paced synthetic 16 MS/s fixtures with bounded waits. Their queues stay bounded, but the generator waits for space instead of losing waveform samples on slower hosts. Both must classify the expected synthetic envelope, record zero sample drops and preserve FFT/storage/lifecycle invariants. No hardware is opened. Instrumented builds retain these same assertions.

Use `-DOVMESH_ENGINE_THROUGHPUT_TESTS=ON` to include the separate, serial `performance` tests in CTest, or run `test_engine --realtime` / `test_engine --detailed --realtime` from an ignored build directory. These preserve real-time pacing and the zero-drop requirement. An offline correctness pass cannot substitute for a throughput pass, and neither qualifies USB or field reception. See [Linux troubleshooting](../docs/operations/linux-build.md#synthetic-engine-tests-and-throughput).

## USB dependency setup and parser regression

Run the standalone USB setup checks from the repository root:

```sh
python3 tests/test_reviewed_usb_configure.py
python3 tests/test_usb_bootstrap.py
```

The CMake checks build synthetic libraries exposing only version accessors and test accepted/rejected dependency prefixes. Helper checks use generated archives and mocked build/download boundaries. They do not download dependencies, initialize USB or open devices. Their generated files stay under ignored `build/` directories. These tests are separate from the application CTest suite.

[libusb_descriptor_regression.c](diagnostics/libusb_descriptor_regression.c) tests malformed and valid descriptor bytes against the actual configured libusb parser under ASan/UBSan, without a USB backend. Follow the [manual compile/run procedure](../docs/security/usb-intake.md#reproduce-the-descriptor-regression); it requires the configured source tree, not just installed headers. The [USB intake](../docs/security/usb-intake.md#validation-and-limits) records completed results and their limits. Neither setup checks nor parser regressions establish live receiver compatibility or overall USB-stack security.

`meshtastic_presets` cross-checks all 17 reviewed presets against vendored schemas and explicit RF parameter vectors. `discovery_decoder` exercises automatic discovery/replay/PHY for every preset at arbitrary frequencies, bounded work, gaps and bad-CRC byte erasure. `automatic_decoder` covers engine classification without fixed profiles, acquisition-scoped recording, Save copy, exports and legacy unknown-state handling. These are offline synthetic checks; shared modem ancestry is not independent on-air validation.

## Discovery throughput regression

Run the optimized `discovery-worker-capacity` diagnostic separately from builds and correctness tests:

```sh
build/native/discovery-worker-capacity --seconds 30 --span-mhz 12.8 --scenario packets --spectrum
```

It generates bounded synthetic input before timing, then submits it at 16 MS/s without waiting for detector queue space. Automatic PHY dispatch is enabled by default, matching ordinary desktop discovery; `--no-decode` isolates waveform discovery cost. The `packets` scenario includes complete 250 kHz/SF11/CR4/5 and 500 kHz/SF11/CR4/8 frames at arbitrary frequency offsets, with expected CRC and byte comparisons. The independent analytic `lora` preamble scenario remains separate. No radio, GPS, keys or recorded IQ files are used.

Check rejected samples, stream resets, source lateness, backlog/drain time, expected versus matched observations, frame results and candidate/result limits. Zero loss in a short run is insufficient if backlog keeps growing. Do not run other CPU-heavy tests simultaneously, or report this diagnostic as USB, GUI, disk, sensitivity or cross-machine qualification. Repeat at the intended survey span and host before making a performance claim.

`discovery_reset` checks actual bounded ring erasure, wraparound and discontinuities, plus lazy repetition confidence against the earlier rolling calculation. `discovery_chirp_screen` compares compacted storage against direct reference statistics, including all global sample phases and invalid input on unused sample phases. These changes preserve the BW/SF search and detection thresholds.
