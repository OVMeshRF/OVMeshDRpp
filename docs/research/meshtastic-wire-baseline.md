# Native Meshtastic wire baseline

Implementation: [protocol.cpp](../../src/protocol.cpp) and
[protocol.hpp](../../include/ovmesh/protocol.hpp). This is a receiver-side,
bounded projection of the pinned RF protocol, not the Meshtastic device API.
It currently supports channel AES-128/256-CTR; recipient PKI and MeshCore are
not implemented in this module. Synthetic tests do not establish on-air
interoperability or physical receiver performance.

## Original sources

Firmware revision: `6d41e279f1f51bd59f687b9d441c1bf47b1594fc`.
Original wire-review protobuf revision: `cf0a84ede1e7a0b7479e90a5e496d30c5dfba707`.
The implementation now uses generated descriptors from official protobuf v2.8.0
(`7b2464c9b8c1521f93852261e4123826e5b25e11`) with Nanopb 0.4.9.2.
Official v2.7.19 (`e1a6b3a868d735da72cd6c94c574d655129d390a`) also supplies independent
compatibility fixtures. Full schema sources, projection options and generated C are vendored. No
firmware, device API, or SDRangel installation is linked. See the
[Nanopb intake](../security/nanopb-intake.md) for exact provenance and bounds.

| Source | SHA-256 | Behavior used |
| --- | --- | --- |
| [RadioInterface.h](https://github.com/meshtastic/firmware/blob/6d41e279f1f51bd59f687b9d441c1bf47b1594fc/src/mesh/RadioInterface.h) | `03a3a8fb3834a3348333a1cc232d8e5f79807ac1ccd3197312417c2f8afd0dd1` | 16-byte RF header, 255-byte frame ceiling, flags |
| [CryptoEngine.cpp](https://github.com/meshtastic/firmware/blob/6d41e279f1f51bd59f687b9d441c1bf47b1594fc/src/mesh/CryptoEngine.cpp) | `b2dcb728fd774797c0228766c7ad226bc6d25b5a7ba9b43bf6d92918c7a84cd6` | `initNonce`, AES-CTR IV and four-byte counter |
| [Channels.cpp](https://github.com/meshtastic/firmware/blob/6d41e279f1f51bd59f687b9d441c1bf47b1594fc/src/mesh/Channels.cpp) | `368f907e5a4155dbbfb389642c15f1c959a26cc03a1a6c8fa69d9c730e765e67` | Channel-name/full-key XOR hash and explicit key-index-1 expansion |
| [Channels.h](https://github.com/meshtastic/firmware/blob/6d41e279f1f51bd59f687b9d441c1bf47b1594fc/src/mesh/Channels.h) | `685cad13114eac523e0d42b7e02b29debb012f7f99e74e31b108869f02448cb3` | Published default key bytes used only when explicitly selected by `AQ==` |
| [Router.cpp](https://github.com/meshtastic/firmware/blob/6d41e279f1f51bd59f687b9d441c1bf47b1594fc/src/mesh/Router.cpp) | `2d014f76e767a92741f7e219b6047aaf2fb70f79045b1704d5d2198dfe800ba9` | Channel-zero unicast PKI convention; decrypted Data handling |
| [mesh.proto](https://github.com/meshtastic/protobufs/blob/cf0a84ede1e7a0b7479e90a5e496d30c5dfba707/meshtastic/mesh.proto) | `2e6df94b777e1e483fe60bbbbbf63a2e8ef6ffd51878cd34bccb1068682f3298` | Data, Position, User, Routing, RouteDiscovery |
| [telemetry.proto](https://github.com/meshtastic/protobufs/blob/cf0a84ede1e7a0b7479e90a5e496d30c5dfba707/meshtastic/telemetry.proto) | `cc7f30598cd3ce2687b38b5437000515d6b0bf60ffb9bc4e1a2a515d8b8224c8` | Telemetry envelope, DeviceMetrics, EnvironmentMetrics |

The selected SDRangel `modemmeshtastic/meshtasticpacket.cpp` at
`866ef1656af7e0923554581afc5c6dd97cbafbaf` informed review of possible
payload projections. Its default-key fallback, plaintext-first parse, raw hex
fallbacks, permissive field aliases, logging, and TX builders are excluded.
This module is newly written against the original wire definitions, with no
copied SDRangel implementation text. Firmware/source provenance remains relevant
to the project GPL-3.0-or-later license and acknowledgement inventory.

Nanopb decodes the selected official messages; the project owns semantic projection and descriptor-driven wire policy. No Python, protoc, generator or package download runs during normal application builds.

## Wire and authorization rules

The header contains little-endian destination, original sender, and packet ID
at offsets 0, 4, and 8. Byte 12 contains hop limit, want-ack, via-MQTT, and hop
start; bytes 13–15 contain channel hash, next-hop suffix, and relay suffix.
These suffixes do not establish the physical transmitter's identity.

For channel encryption the AES IV is packet ID as a zero-extended little-endian
64-bit value, sender as little-endian 32-bit, then four zero counter bytes.
The counter increments from zero in big-endian byte order. Frames are too short
to overflow the four-byte counter, so OpenSSL's 128-bit CTR increment gives the
same blocks within the admitted frame size. The independent nonce example is:

```text
packet ID = 0x78563412, sender = 0x12345678
IV = 12 34 56 78 00 00 00 00 78 56 34 12 00 00 00 00
next block ends with 00 00 00 01
```

The caller supplies an explicit 16- or 32-byte key, entered as full hexadecimal
or canonical padded standard Base64. Exactly `AQ==` explicitly expands index 1
to the published public key. Other one-byte indices, whitespace, URL-safe
Base64, omitted padding and nonzero unused padding bits are rejected.

The keyring has up to 16 configuration records, each bounded to 16 keys at the
protocol API (the current engine/UI installs one key per record). It is independent
of RF frequency/BW/SF lanes. Named records narrow attempts by exact channel-name
hash; explicit key-only records set `restrict_channel_name=false` and do not need
a known name/hash. No keys are discovered, inferred or installed implicitly.
Every record is validated before processing; malformed later records cannot leak
an earlier partial result. An empty keyring never supplies a public fallback.

Eligible identical key bytes are deduplicated with a constant-time comparison in
RAM after scope filtering. Multiple aliases use generic `configured-keyring`
provenance rather than inventing a unique channel. Distinct keys yielding competing
plausible results suppress both content and evidence. No key fingerprint is saved.
The hash and successful schema projection are not authentication.

Channel-zero unicast is conservatively classified `unsupported PKI` before
trying channel keys. A legitimate channel key can also produce hash zero;
such unicast frames are currently unsupported rather than silently treating
the PKI convention as a reliable discriminator. Broadcast hash-zero channel
frames can still be processed with an explicitly matching key.

**AES-CTR channel traffic has no authentication tag.** Successful decryption
cannot be proven cryptographically. A PHY CRC plus the matching configured key
and strict supported schema projection permits retained authorized content,
labeled `likely Meshtastic` and `not authenticated`. It does not authenticate
the sender, detect every ciphertext modification, or eliminate rare false
positive parses. The test suite explicitly demonstrates a text-preserving
alteration that remains unauthenticated.

## Supported projections

| Port | Content | Retained fields |
| --- | --- | --- |
| 1 | Text | Nonempty valid UTF-8, up to 233 bytes |
| 3 | Position | Latitude/longitude, optional altitude and Position.time |
| 4 | Node/User | ID, long/short names, hardware model, role |
| 5 | Routing | Explicit error reason, or supported route request/reply |
| 67 | Device telemetry | Battery percent, voltage, channel utilization, TX air utilization, reported time |
| 67 | Environment telemetry | Temperature, humidity, voltage, reported time |
| 70 | Traceroute | Independent forward/return node lists and both signed SNR lists |

Empty RouteDiscovery requests, including omitted empty Data.payload, are supported.
The pinned [TraceRouteModule.cpp](https://github.com/meshtastic/firmware/blob/6d41e279f1f51bd59f687b9d441c1bf47b1594fc/src/modules/TraceRouteModule.cpp)
(SHA-256 `533bc9281488d086980ab6f6d0a0b3606809616f4da23ae70b36d3f4fe98f424`)
initializes an empty request and serializes it at lines 587–603. This confirms
the previous rejection was a compatibility defect. It does not prove the cause
of any previously discarded live packet. The same source uses `UINT32_MAX` for
an unknown route hop and `INT8_MIN` (-128) for unknown SNR. SNR wire fields are
int32 values in quarter dB; preserve the complete signed value and render -128
as unknown. Forward/return route and SNR lists have independent lengths; no
unobserved node-to-SNR pairing is invented. Each list has its own 32-entry cap.
An empty Routing oneof is unsupported, while explicit error_reason NONE=0 is
retained. Empty route_request/route_reply oneof messages remain distinguishable.

Positions and telemetry times are sender-reported, not receiver observations.
Position latitude/longitude are `sfixed32`, not zigzag; altitude is `int32`;
Position.time is `fixed32`. The separate GPS-solution timestamp is currently
discarded. User.role is field 7. DeviceMetrics battery is `uint32`; value 101
means external power and is not saved as 101 percent. Telemetry.time is
`fixed32`. These pinned definitions take precedence over permissive historical
interpretations in the reviewed SDRangel parser.

The Data envelope retains port, want-response, request/reply IDs and signature
presence with authorized projected content. A v2.8 signature must be absent,
empty, or exactly 64 bytes; presence is **not verification** and does not change
`not authenticated`. Signature bytes, node MAC/public-key bytes, unprojected
fields and unknown payload bytes are discarded.

A valid envelope with unsupported application content is labeled `possible
Meshtastic`, with only its bounded port and signature-presence evidence retained.
It is not promoted to `likely Meshtastic` or stored as authorized content. Bad
CRC, unavailable keys, malformed known fields, and ambiguous key results do not
produce this evidence. Recognizing an envelope still cannot authenticate AES-CTR.

## Parser boundary and deliberate compatibility limits

Nanopb 0.4.9.2 owns decoding, proto3 defaults, optional/oneof presence, packed
arrays and unknown-field skipping. All selected objects have static bounds;
no callbacks, heap allocation, encoder or generator are linked into the app.
The runtime validates UTF-8, disables error strings, uses memory-buffer input
only and limits nesting to eight. Generated descriptor checks apply the project's
stricter unauthenticated-input policy: canonical varints, correct known wire
types and scalar ranges, no duplicate singular fields or ambiguous oneofs,
and no embedded NUL in strings. Unknown fields are structurally checked and
skipped. This stricter policy can reject protobuf encodings other libraries accept.

Frames are capped at 255 bytes, transient plaintext at 256, Data.payload at 233.
Unknown ports and unsupported telemetry variants yield evidence only when the
envelope and any known schema fields pass. Positions lacking coordinates and
telemetry lacking supported measurements are unsupported content. Known malformed
content yields no evidence. Text remains nonempty valid UTF-8 with display control
restrictions; there is no raw-byte fallback. This is selected application coverage,
not support for every Meshtastic application or recipient PKI.

## Cryptographic and persistence boundary

The implementation uses the project-selected OpenSSL 3 libcrypto dependency;
the integrated build currently links the reviewed repository-local 3.5.8 build.
It creates a private OpenSSL library context and explicitly loads the built-in
default provider, without loading an OpenSSL configuration file. Each operation
has its own cipher context. No TLS, network, certificate, or key-file APIs are
used by this module.

Keys are move-only, cleared after moves and on destruction using
`OPENSSL_cleanse`. Transient decryption arrays and decoded Nanopb objects are cleared on every return path.
The caller still owns input frame bytes and the original text-entry buffer and
must enforce their lifetime/clearing policy. This is not an OS-level guarantee
against swap, crash dumps, or process inspection. Authorized projected strings
remain in memory for their intended UI/storage lifetime.

## Validation performed

[test_protocol.cpp](../../tests/test_protocol.cpp) passed with the local static
OpenSSL 3.5.8 library and with AddressSanitizer/UndefinedBehaviorSanitizer.
The checks include independent AES-ECB counter-block construction for both
AES-128 and AES-256; a hand-specified nonce vector; a multi-block message;
typed official schema fixtures; bad PHY CRC; colliding-hash wrong keys; duplicate
keys; unsupported PKI/ports; malformed protobuf/UTF-8; truncation/oversize; and
20,000 deterministic random frames. The OpenSSL intake test separately checks
published NIST CTR known-answer vectors. No operational keys, captured RF bytes,
or actual private content are in these fixtures.

Independent fixtures are serialized by Google's Python protobuf 7.36.1 from
protoc 36.1 output for official schemas 2.7.19 and 2.8.0, then checked through the
native decoder. The 21 artificial fixtures contain no captured RF traffic or
operational data. The generator and inputs are recorded under tests/fixtures and
tools/generate_protocol_fixtures.py; they are not application dependencies.
Continued mutation/fuzz coverage, additional platforms, weak-signal reception,
recipient PKI and MeshCore remain open. See the [upgrade validation](../engineering/quality-and-validation.md).
