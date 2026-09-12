# Native Meshtastic wire baseline

Implementation: [protocol.cpp](../../src/protocol.cpp) and
[protocol.hpp](../../include/ovmesh/protocol.hpp). This is a receiver-side,
bounded classification of the pinned RF protocol, not the Meshtastic device API.
Semantic message interpretation was removed under [ADR-0010](../decisions/0010-metadata-only-surveys.md).
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
| [mesh.proto](https://github.com/meshtastic/protobufs/blob/cf0a84ede1e7a0b7479e90a5e496d30c5dfba707/meshtastic/mesh.proto) | `2e6df94b777e1e483fe60bbbbbf63a2e8ef6ffd51878cd34bccb1068682f3298` | Data envelope; other descriptors are historical fixture/provenance inputs |
| [telemetry.proto](https://github.com/meshtastic/protobufs/blob/cf0a84ede1e7a0b7479e90a5e496d30c5dfba707/meshtastic/telemetry.proto) | `cc7f30598cd3ce2687b38b5437000515d6b0bf60ffb9bc4e1a2a515d8b8224c8` | Historical semantic decoder and fixture provenance; no current payload interpretation |

The selected SDRangel `modemmeshtastic/meshtasticpacket.cpp` at
`866ef1656af7e0923554581afc5c6dd97cbafbaf` informed review of possible
payload projections. Its default-key fallback, plaintext-first parse, raw hex
fallbacks, permissive field aliases, logging, and TX builders are excluded.
This module is newly written against the original wire definitions, with no
copied SDRangel implementation text. Firmware/source provenance remains relevant
to the project GPL-3.0-or-later license and acknowledgement inventory.

Nanopb decodes only the outer Data envelope in the application classification path; the project owns descriptor-driven wire policy. Inner payload bytes remain opaque. No Python, protoc, generator or package download runs during normal application builds.

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

The keyring has up to 16 user configuration records plus one separate public-default
record, each bounded to 16 keys at the protocol API (the current engine/UI installs
one key per record). It is independent
of RF frequency/BW/SF lanes. Named records narrow attempts by exact channel-name
hash; explicit key-only records set `restrict_channel_name=false` and do not need
a known name/hash. No private keys are discovered, inferred or installed implicitly.
Ordinary desktop startup supplies the published public key through its visible,
remembered enable switch; CLI and managed/test callers keep explicit key setup.
Every record is validated before processing; malformed later records cannot leak
an earlier partial result. An empty keyring never supplies a public fallback.

Eligible identical key bytes are deduplicated with a constant-time comparison in
RAM after scope filtering. No profile identity, channel identity or key fingerprint
is returned or saved with the classification. Distinct keys yielding competing
plausible envelopes suppress all evidence. The hash and successful envelope parse
are not authentication.

Channel-zero unicast is conservatively classified `unsupported PKI` before
trying channel keys. A legitimate channel key can also produce hash zero;
such unicast frames are currently unsupported rather than silently treating
the PKI convention as a reliable discriminator. Broadcast hash-zero channel
frames can still be processed with an explicitly matching key.

**AES-CTR channel traffic has no authentication tag.** Successful decryption
cannot be proven cryptographically. PHY CRC, explicit-key processing and a
plausible bounded Data envelope support only tentative classification. They do
not authenticate a sender, detect every ciphertext modification, or eliminate
false positives. Inner payload validity is deliberately not evaluated.

## Classification evidence

The classifier returns a categorical status, classification label, the fixed
`not authenticated` description and, when eligible, only two envelope facts:
numeric application port and signature presence. It does not return sender,
destination, packet/request/reply IDs, profile identity, message contents, node
names, positions, telemetry or routes.

A valid envelope using ports 1, 3, 4, 5, 67 or 70 is labeled `likely Meshtastic`.
Other valid ports from 1 through 65535 are labeled `possible Meshtastic`. These
port labels do not claim that a message of that type was successfully interpreted.
Empty, binary, invalid-UTF-8 or otherwise malformed inner payloads can still
produce classification evidence when their outer envelope is valid. This is a
weaker plausibility check than semantic decoding, and field false-positive rates
have not been established.

The Data payload field must be present except for port 70. The pinned
[TraceRouteModule.cpp](https://github.com/meshtastic/firmware/blob/6d41e279f1f51bd59f687b9d441c1bf47b1594fc/src/modules/TraceRouteModule.cpp)
(SHA-256 `533bc9281488d086980ab6f6d0a0b3606809616f4da23ae70b36d3f4fe98f424`)
constructs an empty traceroute request. This exception checks only outer framing;
no route list or node identifier is extracted. Present empty payload fields on
other ports are accepted without assigning them semantic meaning.

A v2.8 signature must be absent, empty, or exactly 64 bytes. Presence is **not
verification** and never changes `not authenticated`. Signature bytes and all
other unreturned envelope fields are cleared with the transient decoded object.
Bad PHY CRC, unavailable keys, malformed outer fields, cryptographic errors and
ambiguous key results produce no envelope evidence.

## Parser boundary and deliberate compatibility limits

Nanopb 0.4.9.2 owns outer protobuf decoding, proto3 defaults and unknown-field
skipping. Objects have static bounds; no callbacks, heap allocation, encoder or
generator are used by this decode path. Runtime settings enable UTF-8 checks,
disable error strings, use memory-buffer input and limit nesting to eight.
Descriptor checks enforce canonical varints, correct known wire types and scalar
ranges, and reject duplicate singular fields or ambiguous oneofs. These checks
apply to the Data envelope; they do not recursively inspect its bytes payload.
Unknown outer fields are structurally checked and skipped. The stricter outer
policy can reject protobuf encodings other libraries accept.

Frames are capped at 255 bytes, transient plaintext at 256, and Data.payload at
233. The frame header must have a nonzero destination and a sender other than
zero or the broadcast value. Sender and packet ID are used transiently to build
the AES nonce and are not returned. Recipient PKI and MeshCore interpretation
remain unsupported. No raw-byte fallback or semantic payload parser remains.

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
against swap, crash dumps, or process inspection. No semantic strings or message
objects are projected into application results. New recordings retain only
RF/GPS/classification metadata; historical semantic columns and route tables
are excluded from reads, UI and exports without rewriting original recordings.

## Validation

[test_protocol.cpp](../../tests/test_protocol.cpp) exercises independent AES-ECB
counter-block construction for AES-128 and AES-256, a hand-specified nonce vector,
multi-block input, explicit key syntax and authorization bounds, hash collisions,
duplicate keys, competing-key ambiguity, unsupported PKI, malformed outer fields,
signature and port limits, truncation/oversize, and 20,000 deterministic random
frames. Compile-time checks reject a return type exposing semantic content.
Opaque-payload tests cover empty, binary, invalid-UTF-8 and artificial private-text
canaries without projecting their contents. These are bounded regression checks,
not a measured field false-positive rate or a general proof against information
leakage. The OpenSSL intake test separately checks NIST CTR known-answer vectors.

Independent fixtures are serialized by Google's Python protobuf 7.36.1 from
protoc 36.1 output for official schemas 2.7.19 and 2.8.0. The classifier now checks
only envelope port/signature evidence from these 21 artificial fixtures. They
contain no captured RF traffic or operational data. The generator and inputs
under tests/fixtures and tools/generate_protocol_fixtures.py are development
assets, not application dependencies. Historical semantic assertions and older
sanitizer results do not validate the revised boundary; record current test
results with the implementation change.

Storage/report tests additionally check exclusion of synthetic legacy semantic
fields, original-file preservation and metadata-only Save-copy reconstruction.
Continued mutation/fuzz coverage, additional platforms and field reception remain
open. See [quality and validation](../engineering/quality-and-validation.md).
