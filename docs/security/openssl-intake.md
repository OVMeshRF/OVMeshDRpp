# OpenSSL libcrypto source and build baseline

The application pins OpenSSL **3.5.8** and uses static `libcrypto` for its crypto
primitives. This document records source provenance, build configuration and
validation procedures. Validate each intended target separately before
distribution.

OpenSSL 3.5.8 was released on 2026-08-25 and uses Apache-2.0 licensing.
[Official release page](https://openssl-library.org/source/)

## Source integrity

Exact upstream download URLs and digests for the source archive, published
checksum, detached PGP signature and signing-key bundle are retained in
[the source manifest](../../third_party/openssl-source.json). The archive's
recorded SHA-256 matches the published checksum:

```text
a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2
```

The detached PGP signature has **not been verified**. The recorded upstream
primary signing fingerprint is
`B146647E45A7B33947AB226B2A2C87D161692D40`; retaining a fingerprint or checksum is
not publisher authentication. Verify the applicable release signature and trust
chain before relying on it for a distribution. [Signing information](https://openssl-library.org/source/)

Before extraction, require the expected archive prefix and reject absolute or
traversal paths, links and special/device entries. Review `Configure`,
`configdata.pm.in`, the target Makefile template, relevant generators and the
resulting build commands for unexpected downloads, services or privileged/system
installation. This is a bounded build-path review, not a complete source audit.

The manifest also pins the upstream `Configure` and Unix Makefile template, and
the committed smoke-test source. These are **source-file hashes**, not fingerprints
of compiled libraries, whose hashes depend on the build environment. Preserve the upstream
[license](../../third_party/openssl/LICENSE.txt) with the dependency.

## Assisted local setup

The explicit [bootstrap helper](../../tools/bootstrap_openssl.py) automates the recorded configuration for native Linux x86_64/aarch64 and macOS arm64/x86_64. Use `python3 tools/bootstrap_openssl.py --download`, or `--archive PATH` without network access. It uses only Python's standard library; no virtual environment or pip package is needed. See [complete Linux instructions](../operations/linux-build.md).

The helper verifies the pinned archive size/SHA-256 before extraction or build, rejects links, special entries and paths outside the expected source prefix, then checks the recorded build-source hashes. Downloads require an explicit flag, HTTPS and the official release/CDN hosts. Builds and logs stay under ignored `build/`, and only generated public headers, static libcrypto and its license are staged. It validates the existing hardening/NIST smoke test through CMake before making a new prefix available; existing prefixes are left unchanged. These checks do not verify the unverified detached publisher signature or replace target qualification.

Normal CMake configuration selects the prepared local prefix by default, or an explicit `OPENSSL_ROOT_DIR`. It rejects missing or mixed-prefix headers/libraries, nonstatic crypto and an incorrect version. It compiles and links the hardened intake source even when application tests are disabled. Configure does not execute that test or download/build a dependency. Run CTest for the known-answer runtime check. A header/library path and version check is not proof of source provenance; use the recorded source preparation and inspect the final linkage.

## Build configuration

The helper's probe and application share the same prefix-only `OpenSSL::Crypto` target. System `openssl.pc` metadata is not consulted for this archive: it can describe dependencies such as Zlib that are deliberately absent from the reviewed configuration. Platform thread/dynamic-loader libraries remain explicit, and pkg-config remains available independently for USB adapters.

Build static `libcrypto` with its default provider available for AES-128/256-CTR.
Disable shared libraries, DSO loading, engines, dynamic modules, automatic
configuration loading, applications, sockets, HTTP, TLS/DTLS/QUIC, compression and
the legacy provider. No external crypto service or updater is required at runtime.

The compiler, make, Perl and bundled Text::Template are build tools, not application
runtime libraries. The `no-apps`/`no-tests` configuration omits OpenSSL's upstream
applications and full test suite; it must not be presented as passing that suite.

Select the documented OpenSSL Configure target for the intended OS, architecture
and compiler. For example, `darwin64-arm64-cc` targets macOS ARM64. The following
recipe uses that example with a repository-local prefix; choose and validate the
appropriate target for other platforms. It is a build recipe, not a recorded host
configuration or a promise of binary reproducibility across compilers.

From the project directory, after verifying and extracting the pinned archive:

```sh
project_root="$PWD"
openssl_target=darwin64-arm64-cc
cd build/deps/openssl-3.5.8
perl Configure "$openssl_target" \
  no-shared no-dso no-engine no-module no-autoload-config \
  no-apps no-docs no-tests no-sock no-http no-quic no-tls no-dtls \
  no-comp no-zlib no-legacy \
  --prefix="$project_root/build/deps/openssl-3.5.8-local" \
  --openssldir="$project_root/build/deps/openssl-3.5.8-local/ssl" --libdir=lib
make build_generated
make libcrypto.a
```

For this Unix-style target, stage generated `include/openssl/*.h`, `libcrypto.a`
and `LICENSE.txt` under the prefix, preserving the `include/openssl` and `lib`
subdirectories. No global installation is required. Library naming and build
commands may differ on other toolchains.

CMake can use the staged dependency through:

```text
OPENSSL_ROOT_DIR=<project>/build/deps/openssl-3.5.8-local
OPENSSL_INCLUDE_DIR=<project>/build/deps/openssl-3.5.8-local/include
OPENSSL_CRYPTO_LIBRARY=<project>/build/deps/openssl-3.5.8-local/lib/libcrypto.a
OPENSSL_USE_STATIC_LIBS=TRUE
```

## Validation and updates

The committed [smoke test](../../third_party/openssl/intake_smoke.c) checks
four-block AES-128-CTR and AES-256-CTR published examples, including counter carry,
from sections F.5.1/F.5.5 of
[NIST SP 800-38A](https://csrc.nist.gov/pubs/sp/800/38/a/final). Compile-time checks
require configuration-autoload, DSO, engine, socket and HTTP exclusions. Inspect
the resulting binary's dependencies with the target platform's tools and record
build/test outcomes separately. These checks address selected primitive behavior
and linkage; they are not FIPS validation or Meshtastic packet authentication.

From the project directory, an example for the staged Unix-style build is:

```sh
mkdir -p build/deps/openssl-intake
cc -O2 -Ibuild/deps/openssl-3.5.8-local/include \
  third_party/openssl/intake_smoke.c \
  build/deps/openssl-3.5.8-local/lib/libcrypto.a \
  -o build/deps/openssl-intake/test_crypto
build/deps/openssl-intake/test_crypto
```

Keep downloaded archives, extracted sources, binaries and private build logs under
ignored `build/`. For an update, recheck the official patch release and advisories,
verify its digest and signature, review build-path changes, rebuild each intended
target and rerun this smoke test plus application protocol/integration tests.
Do not silently substitute a system crypto version or a floating upstream branch.
