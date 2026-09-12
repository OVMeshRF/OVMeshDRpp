# Ubuntu 0.4.1 package validation

Validation date: 2026-09-12. These are experimental Ubuntu 24.04 packages for
amd64 and arm64. Container checks do not establish physical receiver or desktop
acceptance.

## Build and isolation

Both architectures use the pinned Ubuntu image digests in the [recipe](README.md),
GCC 13.3.0, CMake 3.28.3 and glibc 2.39 in Release mode. amd64 tests run under
architecture emulation; arm64 tests run in the local Linux VM. Compilation and
tests run without networking, host devices, root privileges or capabilities.
Storage fixtures use Linux tmpfs rather than the host bind filesystem.

The reusable runner retains separate architecture caches, verified dependency
prefixes, Docker images and incremental build objects. Each package includes a
source/component inventory and recipe hashes. Matching application source and
pinned upstream archives accompany the release. Build tools and Docker are not
application runtime dependencies.

## Results

| Check | amd64 | arm64 |
| --- | --- | --- |
| Reviewed dependency hashes, headers, versions, linking and AES probes | Passed | Passed |
| Complete CTest suite | 55/55 passed | 55/55 passed |
| Package payload, notices and ELF/RUNPATH inspection | Passed | Passed |
| Runtime-container installation, non-root CLI/demo and removal | Passed | Passed |
| Packaged non-root Xvfb/software OpenGL GUI | Passed | Passed |
| Shared libusb search-path override | Passed | Passed |

The GUI checks render ten synthetic frames. No radios or GPS are opened. The
libusb override check verifies loader selection using a copy of the packaged
library; it does not qualify an independently modified libusb implementation.
Optional real-time throughput tests are not enabled, so these results do not
establish sustained full-span reception performance.

## Package properties and limits

Each package contains four ELF files: desktop, CLI, RAK worker and replaceable
shared libusb. Executables use `$ORIGIN/../lib/ovmeshdrpp` RUNPATH. No absolute
build RPATH, local macOS paths or system OpenSSL dependency is required. Source
manifests and license notices are included; operational data and development
logs are excluded. Minimal container images may intentionally omit installed
`/usr/share/doc` files through their own dpkg path-exclude policy; the package
archive itself contains those files.

The baseline is Ubuntu 24.04 with X11/XWayland, glibc 2.38+ and libstdc++ 13.1+.
OpenGL/X11 libraries and `xdg-utils` are distribution dependencies. A separately
installed default browser is needed to open HTML report previews. There is no
package hook to change USB permissions, install services or change drivers.

Physical Linux desktop/GPU behavior, USB permissions, HackRF/RTL/RAK operation,
GPS, RF characterization and endurance require field testing. Other distributions
and older Ubuntu releases are not qualified. Desktop LoRa discovery and packet
classification are disabled in this release; energy measurements are not packet
counts or transmitter identities.
