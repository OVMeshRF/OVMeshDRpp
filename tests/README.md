# Test-data privacy

The tracked tests contain invented identities, sample coordinates, public protocol examples and deterministic cryptographic vectors. They are test inputs, not live GPS logs, channel secrets or application defaults. Generated files belong under ignored `build/` paths.

- `test_gps.cpp` labels the published RMC/GGA checksum examples and GPSD GN-talker example in `golden()`. Other sentences are constructed parser cases; no USB capture is loaded by these tests.
- Geographic/storage/report tests explicitly construct positions, including zeros, near-equator decimal values, polar limits and wraparound cases. Invalid coordinates deliberately test rejection. They do not establish an operator location.
- [Protocol fixtures](fixtures/README.md) are generated from official versioned schemas with invented values. Known cryptographic vectors and explicitly installed test keys must remain distinguishable from real operational keys. Runtime key normalization may recognize an explicitly supplied public-key shorthand; that is not permission to preload a key.
- [Documentation screenshots](../docs/screenshots/README.md) distinguish synthetic interface examples from reviewed measurement-only report excerpts.

Do not replace these inputs with private captures to reproduce a bug. Prefer a minimal synthetic fixture, document its provenance and review every serialized field. Coordinates and secrets can be encoded in protobuf bytes, JSON, filenames, screenshots or Git history, so a text search alone is insufficient.

## Engine correctness and performance

`engine` and `engine_detailed` run consumer-paced synthetic 16 MS/s fixtures with bounded waits. Their queues stay bounded, but the generator waits for space instead of losing waveform samples on slower hosts. Both must decode the expected synthetic content, record zero sample drops and preserve FFT/storage/lifecycle invariants. No hardware is opened. Instrumented builds retain these same assertions.

Use `-DOVMESH_ENGINE_THROUGHPUT_TESTS=ON` to include the separate, serial `performance` tests in CTest, or run `test_engine --realtime` / `test_engine --detailed --realtime` from an ignored build directory. These preserve real-time pacing and the zero-drop requirement. An offline correctness pass cannot substitute for a throughput pass, and neither qualifies USB or field reception. See [Linux troubleshooting](../docs/operations/linux-build.md#synthetic-engine-tests-and-throughput).
