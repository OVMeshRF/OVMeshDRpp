# Nanopb and Meshtastic schema intake

Nanopb provides the native bounded decoder with generated descriptors from official schemas. It does not add a device API, telemetry service, network listener, updater or application Python runtime.

The selected runtime is **Nanopb 0.4.9.2**, pinned at commit `160d4f09e5fabb2b66aa2dea32d4f38ace2c4b3f`. The official release
archive hash was verified before source adoption. The zlib license is compatible
with this GPL application. Exact source and license records are in
[Nanopb provenance](../../third_party/nanopb/PROVENANCE.md) and
[schema provenance](../../third_party/meshtastic/PROVENANCE.md).

## Why this dependency

A handwritten projection parser can miss valid zero/default-valued protobuf
messages and drift as official fields change. Generated schema descriptors
reduce field-definition duplication while retaining a small C runtime. Google
protobuf's C++ runtime is a larger application dependency; keeping the handwritten
parser requires maintaining protocol type/oneof/presence behavior ourselves.
Nanopb adds two compiled decoder/common C files plus generated descriptors.
The encoder is not adopted or linked. Authentication and retention decisions
remain application responsibilities; successful protobuf parsing is not proof of
the key, sender identity, or intended recipient.

## Review of known advisories

| Upstream advisory | Relevance and disposition |
|---|---|
| [GHSA-p24j-vqcp-x988](https://github.com/nanopb/nanopb/security/advisories/GHSA-p24j-vqcp-x988) | Affected 0.4.0–0.4.9.1; fixed 0.4.9.2. Mixed callback/noncallback oneof members could reinterpret union contents as a callback pointer, even without malloc. Adopted release contains the fix; generated application fields contain no callbacks or pointers. |
| [GHSA-9w99-4pfq-6396](https://github.com/nanopb/nanopb/security/advisories/GHSA-9w99-4pfq-6396) | Affected through 0.4.9.1; 0.4.9.2 adds optional nesting protection. The application explicitly enables `PB_MESSAGE_NESTING_MAX=8`; the release upgrade alone is insufficient. |
| [GHSA-xwqq-qxmw-hj5r](https://github.com/nanopb/nanopb/security/advisories/GHSA-xwqq-qxmw-hj5r) | Older malloc/pointer and delimited-stream memory leak; fixed 0.4.9.1. Adopted decoder does not enable allocation or custom unknown-length streams. |
| [GHSA-7mv5-5mxh-qg88](https://github.com/nanopb/nanopb/security/advisories/GHSA-7mv5-5mxh-qg88), [GHSA-85rr-4rh9-hhwh](https://github.com/nanopb/nanopb/security/advisories/GHSA-85rr-4rh9-hhwh), [GHSA-gcx3-7m76-287p](https://github.com/nanopb/nanopb/security/advisories/GHSA-gcx3-7m76-287p) | Historical pointer/oneof allocation defects; fixes precede the selected release. Pointer/malloc paths are additionally excluded by this build configuration. |

This records reviewed published issues, not a certification that unknown defects
do not exist. Source copies require the same future advisory review and patching
as linked libraries.

## Runtime constraints

CMake passes these ABI-affecting flags publicly through the runtime, generated
schema, and consumer targets:

```text
PB_BUFFER_ONLY=1
PB_VALIDATE_UTF8=1
PB_NO_ERRMSG=1
PB_MESSAGE_NESTING_MAX=8
```

`PB_ENABLE_MALLOC` is **undefined**, not defined as zero, because Nanopb tests it
with `#ifdef`. The decoder consumes bounded in-memory RF plaintext buffers. There
are no generated callbacks/pointers, custom streams, recursive message schemas,
runtime generator execution, or decoder error strings to log. Unsupported
telemetry variants have bounded typed descriptors but do not automatically enter
the authorized-content projection or storage allowlist. The application must
cleanse plaintext and temporary generated objects on every exit path.

Nanopb validates schema representation, not every application constraint. Its
static bytes array padding can permit a byte beyond a nominal option bound on
some architectures, so the application must retain exact byte-length checks.
Duplicate known fields, canonical integer forms, boolean range, and embedded NUL
handling remain deliberate application wire-validation rules. Unknown fields
are skipped; they do not become retained opaque payloads.

## Generation tool chain

Only the checked-in C files enter routine builds. Explicit offline regeneration
uses official protoc **36.1** and the **7.36.1 pure Python protobuf wheel**, with
archive digests matched to official release/PyPI metadata. The tooling requires a compatible CPython interpreter; it does not require a global installation or pip build-script execution.
The script verifies pinned inputs, uses `-I -S`, disables bytecode output, loads
the pure Python implementation from a repository-local path, and disables
Nanopb's fallback discovery/autogeneration. Protoc and Python modules process
reviewed official schemas only, never captured RF data.

Reviewed Python advisory ranges exclude 7.36.1:
[GHSA-8qvm-5x2c-j2w7](https://github.com/protocolbuffers/protobuf/security/advisories/GHSA-8qvm-5x2c-j2w7),
[GHSA-7gcm-g887-7qv7](https://github.com/advisories/GHSA-7gcm-g887-7qv7), and
[GHSA-8gq9-2x98-w8hf](https://github.com/protocolbuffers/protobuf/security/advisories/GHSA-8gq9-2x98-w8hf).
Upstream treats `.proto` input as code and notes that its pure Python backend is
less hardened; choosing it avoids native wheel loading but is not a general
sandbox. See the [upstream security policy](https://github.com/protocolbuffers/protobuf/security).

Regeneration is offline and explicit. The tracked manifest contains URLs for
manual approved acquisition; CMake and the script do not download tools or
perform automatic updates. Existing unexpected or symbolic tool output paths
cause regeneration to fail before extraction.

## Validation and maintenance

Intake checks recorded for this pinned source: official archive hashes matched; copied upstream runtime and
schemas are byte-identical to their reviewed archives; generated output passes
repeat-generation byte comparison; the C runtime and descriptor library compile
in an isolated build with desktop and HackRF support disabled. All selected
generated fields are static, and full `int32` SNR arrays are retained. The application test suite contains independent versioned schema fixtures and malformed-message tests. These establish software checks, not live RF interoperability.

For an update, review upstream advisories/release notes and license changes,
resolve immutable commits, verify release digests where provided, inspect the
source diff and generator execution paths, regenerate with pinned tools, inspect
type/size/optional/oneof changes, and run compatibility and sanitizer tests.
Review and update the local inventory deliberately. A hash match alone does not
establish safe parsing, appropriate retention, or protocol authenticity.
