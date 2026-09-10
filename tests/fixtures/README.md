# Independent Meshtastic compatibility fixtures

These 21 artificial fixtures were serialized by Google Python protobuf 7.36.1
from protoc 36.1 output for the complete official schema sets:

- v2.7.19: `e1a6b3a868d735da72cd6c94c574d655129d390a` (10 fixtures).
- v2.8.0: `7b2464c9b8c1521f93852261e4123826e5b25e11` (11 fixtures).

`tools/generate_protocol_fixtures.py` defines the artificial inputs. The checked-in
`.inc` files contain serialized Data envelopes, not captured traffic, secrets,
or RF ciphertext. `test_protocol.cpp` wraps them in independent AES-ECB counter
construction and validates the production decoder. Both schema versions cover
empty initiating traceroutes, packed forward/return routes with signed SNR,
explicit routing NONE/request/reply, text, User, Position, telemetry, and an
unsupported port. The 2.8 fixture also checks signature presence without claiming
signature validity. The hand-built adversarial tests remain separate.

Reproduction requires the reviewed isolated tools and version-specific Python
modules described in [Nanopb intake](../../docs/security/nanopb-intake.md). For
example, from this repository, run a separate process for each version:

```sh
python3 -I -S tools/generate_protocol_fixtures.py --schema-dir build/schema-tools/python-schemas/v2.8.0 --protobuf-dir build/schema-tools/python --version 2.8.0 --output tests/fixtures/meshtastic-2.8.0.inc
```

The normal CMake build uses only the checked-in bytes; it does not invoke this
script or require Python/protoc. These are schema interoperability fixtures,
not full firmware execution or RF-performance tests.
