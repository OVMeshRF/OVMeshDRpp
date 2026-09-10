# Test-data privacy

The tracked tests contain invented identities, sample coordinates, public protocol examples and deterministic cryptographic vectors. They are test inputs, not live GPS logs, channel secrets or application defaults. Generated files belong under ignored `build/` paths.

- `test_gps.cpp` labels the published RMC/GGA checksum examples and GPSD GN-talker example in `golden()`. Other sentences are constructed parser cases; no USB capture is loaded by these tests.
- Geographic/storage/report tests explicitly construct positions, including zeros, near-equator decimal values, polar limits and wraparound cases. Invalid coordinates deliberately test rejection. They do not establish an operator location.
- [Protocol fixtures](fixtures/README.md) are generated from official versioned schemas with invented values. Known cryptographic vectors and explicitly installed test keys must remain distinguishable from real operational keys. Runtime key normalization may recognize an explicitly supplied public-key shorthand; that is not permission to preload a key.
- [Documentation screenshots](../docs/screenshots/README.md) distinguish synthetic interface examples from reviewed measurement-only report excerpts.

Do not replace these inputs with private captures to reproduce a bug. Prefer a minimal synthetic fixture, document its provenance and review every serialized field. Coordinates and secrets can be encoded in protobuf bytes, JSON, filenames, screenshots or Git history, so a text search alone is insufficient.
