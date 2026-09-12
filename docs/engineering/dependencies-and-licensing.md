# Dependencies and licensing

Status: experimental native application dependency inventory. The combined build uses GNU GPL version 3; original project code is GPL-3.0-or-later. Third-party terms remain intact. Exact pins, grants and notices are in [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md); precautions and limits are in [security intake](../security/security-and-privacy.md).

| Area | Actual choice | Boundary |
|---|---|---|
| Core/build | C++20 and CMake | Compiler, platform SDK and thread runtime; no application interpreter |
| LoRa | Selected SDRangel-derived logic | No SDRangel, SDR++ or Qt runtime |
| FFT/channelizer | Owned native code | No FFTW/KISS FFT package; project owns correctness/performance work |
| Protobuf | Nanopb 0.4.9.2 + generated official v2.8.0 Meshtastic descriptors | Static bounded decode-only C runtime; no Python/protoc/generator at build or runtime. [Intake and tooling provenance](../security/nanopb-intake.md) |
| Crypto | OpenSSL libcrypto | Maintained primitives, isolated local static build; no libssl application or external provider/config loading |
| Sessions | SQLite amalgamation | Embedded storage, no server/extensions, typed schema; plaintext files |
| Desktop | Dear ImGui + GLFW + OpenGL | Existing vendored sources/backends and OS graphics; local system-font lookup plus built-in fallback, no font download |
| USB | Repository-local static libhackrf 2024.02.1 + reviewed static librtlsdr 2.0.3 + shared libusb 1.0.30 | One reviewed USB prefix for both SDR adapters; no host fallback, upstream utilities or implicit installation; [USB intake](../security/usb-intake.md), [RTL intake](../security/rtlsdr-intake.md). Packaging must inventory actual closure |
| RAK5146 USB/LBT | Selected Semtech sx1302_hal 2.1.0 + bundled TinyMT32 | BSD-licensed sources with pinned original/adopted hashes; isolated local C worker, native POSIX serial API, no libusb dependency, packet forwarder, network listener, MCU flasher or new runtime framework; [provenance](../../third_party/sx1302_hal/PROVENANCE.md) |
| GPS | Owned NMEA, device discovery and OS serial APIs | Metadata enumeration via macOS IOKit/CoreFoundation, Windows SetupAPI or Linux serial/sysfs; no serial-port traffic probing, gpsd or external GPS package |
| Desktop preferences | Owned bounded format 8 + OS file APIs | Existing OpenSSL random generation; private atomic receiver/source/display/settings persistence, including a public-key enable switch; no settings framework, private keys, coordinates or content |
| Session copy / image capture | Existing SQLite APIs + owned metadata reconstruction and PNG writer | Private new local files; allowlisted metadata from a consistent saved-session snapshot or bounded spectrum/waterfall image; no backup framework, image codec package or OS-dialog dependency |
| Cloud/remote | None | No listener, telemetry, online map, updater or source synchronization |

Using maintained cryptographic and USB implementations avoids disproportionate maintenance risk. Owned DSP/parsing reduces packages but increases testing and update responsibility. GPS setup uses native OS metadata; recognized or remembered enabled GPS may connect on ordinary hardware-oriented startup, while RF stays stopped. Each platform needs its own runtime and dependency-closure validation.

The RAK worker builds on macOS and eligible Linux/glibc 2.34+ systems with safe child-process descriptor closure. It is disabled on Windows and unsupported POSIX configurations; this does not remove the existing SDR build paths. The copied HAL subset preserves its BSD-3-Clause license and TinyMT32 notices, disables transmit entry points and MCU bootloader/reset commands, bounds USB transactions, and suppresses upstream packet/identifier logging. Volatile radio initialization arrays are included; persistent MCU firmware and flashing tools are not. Corresponding source and local changes ship with the source inventory. Linux and full application hardware qualification remain separate from a successful build or isolated macOS checks.

## Version 0.4.0 desktop assets and session workflow

The desktop uses locally installed system sans-serif and monospace fonts through the existing Dear ImGui font path, with the existing licensed built-in fallback. OS font files are not vendored, downloaded or copied into the application bundle; their respective font/OS licenses remain applicable. Do not add them to a future package without reviewing redistribution rights.

The bounded PNG writer is project-owned and emits uncompressed PNG data. Saved-session copies reconstruct permitted metadata from a consistent SQLite snapshot into a fresh database; database pages and historical semantic content are not copied. Only recordings marked with the current metadata-only policy can be copied. This adds no dependency or server. Both features reuse the existing in-app local file chooser, not a new native-dialog framework. These desktop features add no further third-party package, cloud service, plugin host, auto-updater or network asset source. **Settings > About & licenses** remains the current in-app notice entry point.

## Intake and maintenance

State the need and smaller alternatives before expanding dependencies. Retrieve official source separately from building it; pin revision, archive/selected-file hashes, license and enabled features. Verify available publisher checksums/signatures and record anything not verified. Inspect build scripts for downloads, process execution, generators and install side effects. Do not run installers or change system packages implicitly.

Routine CMake configure/build/startup must not fetch packages. Compile only needed targets and verify the reviewed local inventory offline. Recheck official advisories and exposed code paths for each release, then update source pins, notices, tests and validation together. A hash match, small footprint or reputable upstream is not proof that software is uncompromised; an old pin does not remove the obligation to apply reviewed security fixes.

## Distribution gate

Local build authorization does not authorize distribution. Before shipping, inventory static/dynamic libraries on each platform, include full notices and corresponding source/build materials, satisfy applicable GPL/LGPL and other terms, and validate installer/signing/update behavior. Assign maintainers for releases, dependency advisories and private security reports. Acknowledgments alone are insufficient, and Mac tests do not establish Windows/Linux support.

The current build requires reviewed **libusb 1.0.30**, prepared locally with static libhackrf 2024.02.1; host libusb 1.0.29 is no longer a build fallback. Earlier libusb versions have descriptor-parser memory-safety defects fixed upstream in [PR 1814](https://github.com/libusb/libusb/pull/1814) and included in [1.0.30](https://github.com/libusb/libusb/releases/tag/v1.0.30). A different version or security-backported build requires a separate source review and deliberate pin update; a version string alone does not establish backport status. CMake requires both SDR adapters to use the same reviewed shared libusb, without USB access or automatic downloads. See [source pins, helper behavior and validation limits](../security/usb-intake.md).

Recheck advisories and inspect the actual direct/transitive loader closure before release. Shared libusb preserves a replaceable library boundary for LGPL distribution, but it does not by itself satisfy all source, notice or replacement obligations. The update leaves installed host libraries unchanged and does not qualify every supported platform.

## Source attribution and distribution limits

Selected SDRangel files preserve their original copyright, GPL grant, pinned revision and adaptation notes. SDR++ is interface inspiration; no SDR++ runtime or source is included. No MeshCore decoder is currently linked. Meshtastic schemas, Nanopb, UI components, storage, cryptography and USB components retain their own licenses.

Settings > About & licenses and CLI `--licenses` expose embedded terms. Source publication alone does not satisfy every obligation for a separately distributed binary. Before binary release, inspect the actual static/dynamic dependency closure; preserve notices; provide matching Corresponding Source, patches, generated schemas and build/install scripts; satisfy applicable LGPL replacement/relinking requirements; and validate each supported platform.

The retained component licenses and source manifests are authoritative for this inventory. License compatibility is not a security certification or proof of RF compliance. See [third-party notices](../../THIRD_PARTY_NOTICES.md).
