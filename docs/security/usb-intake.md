# Reviewed local USB dependencies

HackRF and RTL-SDR builds use **shared libusb 1.0.30** from one repository-local prefix. HackRF also uses **static libhackrf 2024.02.1** built against that libusb. This replaces host-library selection for new application builds without changing the operating system's installed packages. The RAK5146 worker uses its separate pinned Semtech HAL and native serial transport; it does not depend on libusb.

## Pins and provenance

The authoritative source URLs, archive sizes, SHA-256 values and selected build-source hashes are recorded in [usb-source.json](../../third_party/usb-source.json). The inputs are the upstream libusb 1.0.30 release and HackRF 2024.02.1 release. Only the libhackrf library target is built from the HackRF distribution; its utilities and firmware are not part of this dependency build.

The libhackrf 2024.02.1 pin preserves the library baseline used for the existing HackRF One integration while updating its USB dependency. It is not the newest HackRF release: [upstream releases](https://github.com/greatscottgadgets/hackrf/releases) list 2026.01.3. Upgrading libhackrf and qualifying newer hardware/firmware combinations is separate follow-up work. Retaining this pin does not establish support for all HackRF-family hardware or firmware, and no firmware is installed by this application.

The manifest records a libusb archive checksum match to the official release asset and a HackRF archive checksum match to the independent packager reference linked there. These corroborate the selected archive bytes; detached publisher signatures were not verified.

The libusb update includes the descriptor-parser bounds fixes in [upstream PR 1814](https://github.com/libusb/libusb/pull/1814), included in the [1.0.30 release](https://github.com/libusb/libusb/releases/tag/v1.0.30). The [CVE-2026-23679 record](https://github.com/CVEProject/cvelistV5/blob/main/cves/2026/23xxx/CVE-2026-23679.json) identifies affected libusb versions before 1.0.30. This is a reason for the pin, not a claim that every USB input path is free of defects. Hash verification detects changes relative to the reviewed intake; it is not independent publisher authentication, an upstream-compromise check or reproducible-binary verification. No publisher-signature verification is claimed here.

## Explicit local preparation

From the repository root on native macOS/Linux arm64 or x86_64:

```sh
python3 tools/bootstrap_usb.py --download
```

For offline preparation, provide both exact archives identified by the manifest:

```sh
python3 tools/bootstrap_usb.py --libusb-archive /path/to/libusb-1.0.30.tar.bz2 \
  --hackrf-archive /path/to/hackrf-2024.02.1.tar.xz
```

The helper uses Python's standard library and the native C compiler, make and CMake. It verifies the pinned archives before building, bounds and validates extraction, and stages the libraries, headers, notices and a local hash receipt. The default prefix is `build/deps/usb-1.0.30-local`; `--prefix` chooses a different repository-local destination. Existing prefixes are refused rather than replaced. Build sources and logs remain in ignored `build/deps/usb-build-*` directories.

libusb is built shared with static output, examples, tests and system logging disabled. On Linux, `--disable-udev` selects the netlink backend without installing udev rules or adding a libudev development dependency. HackRF builds only `hackrf-static`, with udev-rule installation disabled. The helper does not run a system installation, change drivers or device permissions, initialize USB or open receivers. Network access occurs only when `--download` is explicitly selected; ordinary CMake configuration and builds never invoke the helper or fetch these dependencies.

## Application linkage

CMake defaults to the prepared prefix. Use `-DOVMESH_USB_ROOT_DIR=/absolute/path/to/prefix` for another reviewed build. Headers and libraries must resolve inside that prefix, and its `ovmesh-usb-intake.json` receipt must match the selected files. Shared libusb and a regular static libhackrf archive are required; host fallback and mixed header/library paths are rejected. The receipt detects stale or mixed local inputs but is not a signature or independent source-provenance proof.

The native CMake probe compiles, links and calls library-version accessors without USB initialization, enumeration or device access. A successful version check does not test receiver communication. Configure with `--fresh` or a separate build directory when changing inputs. `OVMESH_ENABLE_HACKRF=OFF` and `OVMESH_ENABLE_RTLSDR=OFF` independently omit adapters; with both off, this USB dependency prefix is not required. See [build instructions](../operations/deployment.md#build).

## License and distribution

libusb retains **LGPL-2.1-or-later**; libhackrf retains its **BSD-3-Clause** library terms. See [third-party notices](../../THIRD_PARTY_NOTICES.md), [libusb license](../../third_party/LIBUSB_COPYING.txt) and [libhackrf notice](../../third_party/libhackrf-NOTICE.txt). The broader HackRF distribution contains differently licensed tools/firmware; the library grant must not be applied to those components.

Shared libusb preserves a library-replacement boundary. Binary packaging must still provide matching source/build materials and complete notices, preserve applicable replacement/relinking rights, and verify the actual loader paths and runtime dependency closure. Copying the main executable alone is not a validated package. No signed, notarized or cross-platform redistributable package is established by this intake.

## Validation and limits

The following checks completed for this update on **macOS ARM64**, using the reviewed source pins above:

| Check | Observed result and scope |
|---|---|
| Release application build | Succeeded with HackRF, RTL-SDR and RAK5146 enabled |
| Native library-version probe | Reported libusb 1.0.30 without USB initialization or enumeration |
| Application CTests | 51/51 passed |
| USB setup regressions | 44/44 passed: 33 CMake configuration cases and 11 helper cases |
| Source-publication boundary and audit tests | 18/18 passed; these check source selection/auditing, not runtime USB behavior |
| `otool` dependency-closure inspection | Application closure contained repository-local libusb 1.0.30 and Apple system libraries, with no host libusb or dynamic libhackrf dependency |
| Retained libusb license | `LIBUSB_COPYING.txt` matched the 1.0.30 source `COPYING` byte-for-byte |
| Actual descriptor-parser ASan/UBSan regression | Malformed interface and interface-association descriptors, plus a valid interface descriptor, passed against the configured 1.0.30 `descriptor.c` |

The parser diagnostic uses synthetic bytes and no USB backend. Separate negative controls reverted each relevant fix individually: one exposed the nonexistent-endpoint invariant and the other produced an ASan heap-buffer-overflow report. These controls demonstrate that the diagnostic detects the two regressions; they are not a complete build or test of libusb 1.0.29. Earlier host-library validation does not qualify the new binaries.

### Reproduce the descriptor regression

After preparing the dependencies, set `USB_SOURCE` to the helper's **configured libusb source directory**, containing generated `config.h` and `libusb/descriptor.c`. This is the source directory, not the installed library prefix. From the repository root, with a compiler supporting AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
USB_SOURCE=/path/to/configured/libusb-1.0.30
cc -std=gnu11 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$USB_SOURCE" -I "$USB_SOURCE/libusb" \
  tests/diagnostics/libusb_descriptor_regression.c \
  -o build/usb-descriptor-regression
build/usb-descriptor-regression
```

The expected result is a zero exit status, the descriptor-regression success message, and no sanitizer report. The [diagnostic](../../tests/diagnostics/libusb_descriptor_regression.c) includes the actual upstream parser with its LGPL notices intact, supplies no USB backend, and aborts on an unexpected control-transfer call. It is a manual parser check, not a receiver test or a complete libusb security audit. See [USB setup test commands](../../tests/README.md#usb-dependency-setup-and-parser-regression) for the separate helper/CMake checks.

The helper targets native macOS/Linux arm64 and x86_64; other architectures, cross-compilation and Windows dependency preparation are not qualified here. Linux device permissions, kernel-driver ownership, desktop behavior and live reception remain separate acceptance work. No USB reception, device compatibility, throughput, endurance or hardware regression result is claimed for this update.

Before release or another dependency change, refresh upstream advisory review, inspect changed parser and build paths, update pins and notices together, rerun the affected offline tests, and inspect the exact shipped dependency closure. See [dependency maintenance and distribution checks](../engineering/dependencies-and-licensing.md#distribution-gate).
