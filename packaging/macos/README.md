# Apple Silicon package preparation

This workflow produces an experimental OVMeshDRpp 0.4.1 app and a
drag-to-Applications disk image. It targets **macOS 13.0 and later, arm64 only**.
The candidate was built and tested on macOS 26.3 (25D125), Apple clang 16.0.0,
macOS 15.2 SDK, CMake 4.1.1. macOS 13 runtime acceptance, a clean-machine test,
Intel/Universal support and packaged hardware operation are not established.

## Build

Use a clean source checkout and repository-local inputs/prefixes. Supply the
three exact archives pinned in `third_party/openssl-source.json` and
`third_party/usb-source.json` under `build/inputs/`. The helpers verify them.
No system-installed third-party libraries are reused.
Previously reviewed repository-local prefixes can be reused with CMake
`-DOVMESH_USB_ROOT_DIR=PATH` and `prepare.py --usb-prefix PATH`; provide matching
archives with `--inputs PATH`. CMake verifies dependency receipts before reuse. `--relocatable` uses neutral compiled-in
OpenSSL directory strings while retaining the reviewed disabled-loading flags
and repository-local staging. Use new dependency and CMake directories when
changing this option; do not reuse the previous CMake cache.

```sh
export MACOSX_DEPLOYMENT_TARGET=13.0
python3 tools/bootstrap_openssl.py --archive build/inputs/openssl-3.5.8.tar.gz \
  --relocatable --prefix "$PWD/build/deps/openssl-3.5.8-relocatable" --jobs 4
python3 tools/bootstrap_usb.py --libusb-archive build/inputs/libusb-1.0.30.tar.bz2 \
  --hackrf-archive build/inputs/hackrf-2024.02.1.tar.xz --jobs 4
cmake -S . -B build/native-release -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DOPENSSL_ROOT_DIR="$PWD/build/deps/openssl-3.5.8-relocatable"
cmake --build build/native-release --parallel 4
ctest --test-dir build/native-release --output-on-failure
python3 packaging/macos/prepare.py
hdiutil create -volname 'OVMeshDRpp 0.4.1 arm64' -srcfolder build/macos-package/image \
  -format UDZO -ov build/macos-package/OVMeshDRpp-0.4.1-macos-arm64-review.dmg
```

Use a fresh output directory for each candidate; `prepare.py` refuses existing
output. It stages only the app, required shared library, worker, license texts,
committed source archive, the three verified upstream archives, and public
package documentation. Tests, local settings, logs and recordings are excluded.
A clean Git worktree is required so corresponding source matches the package
recipe. The source directory must be the root of its own Git repository; an
enclosing repository is refused. The distributed source tarball contains no
Git history. After extraction, initialize a new isolated repository in the
extracted root and commit only those supplied source files before building:

```sh
git init
git add .
git commit -m "Initialize supplied OVMeshDRpp source"
```

Do not copy parent/private Git history. Run these commands before creating
build inputs, and verify the staged file list before the initial commit. The default output is `build/macos-package`. No system installation,
Developer ID signing, notarization, or device access is performed by the script.
The temporary ad-hoc signatures permit ordinary local build validation on arm64;
they provide no Developer ID trust or notarization.

If a restricted build environment blocks the `sysctl` command-length probe,
libtool may leave `max_cmd_len` empty and report partial-link failures. Use an
environment that permits this build probe; do not modify the upstream archive
to work around an environment failure.

## Bundle and library replacement

The app executable and `ovmesh-rak-worker` are in `Contents/MacOS`.
`Contents/Frameworks/libusb-1.0.0.dylib` is the only bundled shared dependency;
its loader path is `@executable_path/../Frameworks/libusb-1.0.0.dylib`.
OpenSSL libcrypto, libhackrf, librtlsdr, SQLite, Nanopb, ImGui, GLFW, selected DSP
and selected Semtech HAL are linked statically into their appropriate binary.
All other dynamic dependencies must be Apple system libraries/frameworks.
Full notices are in `Contents/Resources/Licenses` and in About & licenses.

`Sources/` on the image contains the committed application source and build
scripts plus the exact upstream source archives used for libcrypto, libusb and
libhackrf. Preserve these materials alongside every redistributed binary.
The application source includes the other selected dependencies and their
modifications, manifests and licenses. Python, CMake, Perl, make and the Apple
compiler/SDK are build tools; Python is not an application runtime dependency.

libusb remains a replaceable shared library. To use a modified compatible
libusb, work on a copy of the app, build the replacement for arm64/macOS13+
from the included source, copy it to the same Frameworks path, and set its
install name to `@rpath/libusb-1.0.0.dylib` using `install_name_tool -id`.
Modified code invalidates the original signature. Sign the replacement and
then the copied app using your own identity, or use ad-hoc signing for your own
local build. The included complete source/recipe also permits rebuilding the
whole app. No signing key from the original distributor is necessary for a
local rebuild. There is no custom signature/hash check in the application.
Do not disable Gatekeeper or strip quarantine. Local rebuilding/resigning does
not retain the distributor's notarization or establish trust in modified code.

## Signing review and approval gate

The proposed release uses Developer ID Application signatures with secure
timestamps and hardened runtime (`codesign --options runtime --timestamp`).
No additional entitlements are proposed: no App Sandbox, JIT, executable-memory,
DYLD-environment or library-validation exceptions. Non-sandboxed local USB and
serial access use existing OS APIs. Sign nested libusb and the worker before
the outer app. Verify each signature and the final bundle, then recreate and
sign the DMG. **Obtain owner approval for the exact staged candidate before any
Developer ID signing or submission to Apple.** Signing identity and private-key
availability must be checked in the login Keychain; never export private keys.

After that approval, submit the signed DMG using the existing credentials:

```sh
xcrun notarytool submit PATH_TO_SIGNED_DMG --wait \
  --keychain-profile 'OVMesh-Notarization' \
  --keychain "$HOME/Library/Keychains/login.keychain-db"
```

Record the result; staple and validate approved artifacts. An app-level ticket
may require stapling the app before recreating/finalizing the DMG. Keep notarized
app and DMG payloads consistent and verify their final hashes. Gatekeeper and
installation acceptance remain separate checks. Do not install in
`/Applications` without authorization for the reviewed installation test.

## Safe package checks

Run `Contents/MacOS/OVMeshDRpp --ui-smoke 120` for passive frames and
`--demo --ui-smoke 180` for synthetic reception. These explicit modes do not
load ordinary preferences or automatically open GPS; they must not be combined
with `--settings-directory`. Keep the working directory and any explicit
synthetic `--session` path under an isolated test folder outside the staged app.
Never use no-argument ordinary startup as a packaging smoke test: it can open
an enabled recognized GPS. Do not access radios for packaging.

Check Mach-O arm64/minimum-version records, all load paths/RPATHs, bundle
structure, signatures, notices and package contents. Copy the app to a new
staging folder and repeat the safe launch there. Inspect the DMG read-only;
its top-level Applications link is only a drag destination, not installation.
These synthetic tests do not qualify receiver hardware or full-range automatic
waveform-to-payload dispatch. ADR-0010 metadata-only behavior is unchanged.

### Final release metadata

The review image deliberately says ad-hoc/unnotarized. After the separately
approved nested/app Developer ID signing, run the following on the **staging
folder**, before creating the submission DMG:

```sh
python3 packaging/macos/finalize_metadata.py --image build/macos-package/image \
  --team-id REVIEWED_TEAM_ID
```

This requires valid timestamped Developer ID hardened-runtime signatures for
all three binaries and the expected team, then updates only the external
Read Me and manifest hashes. It does not sign or submit anything, and it leaves
notarization explicitly unverified. After Apple acceptance, staple/validate the
app and rerun with `--notarized-app`; that option also requires successful app
Gatekeeper assessment. Then recreate and sign the final DMG, complete its
notarization/stapling/assessment, and record final DMG and app-archive hashes in
external release evidence. Never claim a DMG is notarized merely because its
app passed. Do not modify the signed app after sealing it; any changed source
materials or metadata require recreating the containing DMG. Ship source/build
materials matching the final integrated release and preserve the prior review
artifacts as private evidence.
