# Experimental Ubuntu packages

OVMeshDRpp 0.4.1 packages target Ubuntu 24.04, separately for amd64 and arm64.
They contain the native desktop application, CLI and experimental separate
RAK5146 worker. HackRF and RTL-SDR adapters are enabled. Linux hardware operation,
native desktop integration, GPS and sustained RF performance remain unvalidated.
The desktop is spectrum-only; LoRa detection and decoding are disabled.
Field characterization remains incomplete.
No semantic message, node, route or content display/retention is provided.

## Installation and removal

From the directory containing the package for your architecture:

```sh
sudo apt install ./ovmeshdrpp_0.4.1-1_amd64.deb
OVMeshDRpp
# Or: ovmesh-cli --help
sudo apt remove ovmeshdrpp
```

For arm64, substitute `arm64` in the filename. Docker is not needed to run the
application. Use X11 or XWayland and your distribution's OpenGL drivers. The
launcher uses the system science icon when available; no external fonts or icons
are bundled. Ordinary startup can connect an enabled, recognized GPS and leaves
RF stopped. Use `OVMeshDRpp --ui-smoke 10 --demo` for a bounded synthetic check
that does not open radios or GPS and does not load ordinary preferences.

No udev rules, device permissions, services or kernel-driver changes are installed.
Use the distribution's documented USB/serial access policy; do not launch as root.
Removal leaves user-owned settings and recordings in place. Preserve recordings
and use isolated preferences before trying a different application version.

## Licenses and matching source

Full installed notices are under `/usr/share/OVMeshDRpp`, including `LICENSES.txt`.
This program is GPL-3.0-or-later; third-party terms are listed in the notices and
source manifests. The binaries statically incorporate the reviewed libcrypto,
libhackrf, librtlsdr, SQLite, Nanopb, ImGui/GLFW and DSP sources. The worker includes
the reviewed Semtech HAL subset. Exact component revisions are in the supplied
source manifests and the matching companion source archive. That archive must be
distributed alongside the binaries and include the application source, the pinned
OpenSSL/libusb/libhackrf archives and these packaging/build scripts. An application
source ZIP alone does not contain all corresponding source.

libusb 1.0.30 is dynamically loaded from `/usr/lib/ovmeshdrpp`. Its LGPL-2.1-or-later
license is included. It is not embedded into the executables. Users may rebuild
and replace it with a compatible modified shared library, or select one using
`LD_LIBRARY_PATH=/absolute/path/to/modified/lib OVMeshDRpp`. The executables use
`DT_RUNPATH`, so this environment override takes precedence. The replacement must
preserve the `libusb-1.0.so.0` ABI/SONAME and resolve its own dependencies. Preserve
the original library for rollback; package upgrades can replace installed files.
No package signature or integrity enforcement prevents such a replacement.

## Repeatable local build

With Docker running and the three pinned upstream archives in `build/inputs`,
commit the reviewed source and run from the checkout root:

```sh
python3 packaging/linux/build-local.py --arch all
# Or build only one architecture:
python3 packaging/linux/build-local.py --arch amd64
```

The helper verifies source archive hashes, exports only the committed public-source
allowlist, and caches the architecture-specific Docker images, dependency prefixes
and Ninja objects under `build/linux-package-cache`. Unchanged source files retain
their timestamps. Later builds reuse this work and rerun the full tests plus
isolated package installation, synthetic launch and removal checks. It does not
access USB devices, install anything on the host or publish artifacts.

Packages and checksums are in `build/linux-package-cache/ARCH/output`; private
validation logs are in the adjacent `evidence` directory. Preserve a release's
artifacts separately before rebuilding the same version. Runtime report opening
uses the distribution's `xdg-utils`; a desktop browser must be installed separately.

Image tags include the Dockerfile digest. Use `--refresh-images` deliberately to
refresh Ubuntu packages and then requalify the build; refreshing is not automatic.
Pinned application dependencies are rechecked by CMake before reuse. The cache is
not a bit-for-bit reproducibility claim: apt package versions and toolchain evidence
are recorded. A lock prevents simultaneous builds from changing shared source.
If interrupted, confirm no build container remains before removing the empty
`build/linux-package-cache/runner.lock` directory and retrying.

## Build recipe

Use only the architecture-specific reviewed Ubuntu 24.04 base digests:

| Target | Base image |
| --- | --- |
| amd64 | `ubuntu@sha256:a61567bd31828687156d735ea8eb01ba4e37636e225dd6a48ba94136a70d9d61` |
| arm64 | `ubuntu@sha256:ec0b1c9058e44c837a21c3f9d8a3d5e9aaa94ed28edceb18e154af5efecf0950` |

Build the `builder` target of `Dockerfile` with `--platform linux/ARCH` and
`--build-arg BASE_IMAGE=DIGEST`. It obtains build tools from Ubuntu's signed package
repositories. Apt packages are version-recorded, not pinned to an immutable apt
snapshot; bit-for-bit rebuilds are not claimed. The builder records exact packages
in `/builder-packages.txt` and `/runtime-packages.txt`.

Export the matching source without Git metadata or private build data. Mount that
source read-only at `/src`, a separate writable architecture-specific build tree
at `/src/build`, these packaging files read-only at `/packaging`, and the three
pinned archives read-only at `/inputs`. Use an unprivileged build user, no devices,
no Docker socket, no host networking, and `--network none`. Mount native Linux
tmpfs at `/src/build/fixtures` and `/src/build/publication-review` for synthetic
test fixtures. Docker Desktop macOS bind-mount permission behavior does not satisfy
all SQLite fixture assumptions. Generated CTest commands are copied into the
tmpfs fixture tree without changing test assertions or executable paths.

The source exporter must add `source-identity.json` to the exported root, with
exact `commit` and `tree` fields from `git rev-parse HEAD` and
`git rev-parse 'HEAD^{tree}'`. The package inventory reads this identity and hashes
the separately mounted packaging materials. Run:

```sh
sh /packaging/build.sh
sh /packaging/package.sh
```

The helpers verify archive pins, build local static libcrypto/libhackrf and shared
libusb, and run non-device configuration probes. OpenSSL uses `--relocatable` and
a fresh `build/deps/openssl-3.5.8-relocatable` prefix selected explicitly by CMake;
neutral compiled-in paths preserve disabled loading without embedding build paths.
Release application builds use
the checked-in schemas and native synthetic CTests. Packages install relative
library search paths, exact notices and a desktop launcher; package dependency
versions are derived from ELF symbols with explicit dynamically loaded desktop
prerequisites. Detailed private build evidence remains under `build/evidence`;
only the deliberately public component inventory enters the package.

The normal build runs the full test suite. The explicit `crypto-relink` argument
runs eight crypto/protocol/engine/storage tests and is appropriate only after a
full platform qualification when the sole change is OpenSSL's compiled-in path
strings. It is not a substitute for a platform's first complete test run.
