# Ubuntu package validation

Validation date: 2026-09-11. These are experimental unsigned Ubuntu 24.04 packages,
not native desktop, USB, GPS or RF acceptance results.

## Source and build configuration

Initial amd64 qualification used source commit
`4b7eef4b3238cde9d9fa3fd6393702a9b2adfb7e`, tree
`ca134ddb4aa1e79cc9d46ac3dcb79a8b0e368541`.
Final binaries on both architectures used exported commit
`130e4cc52854bc4955585f1c0da066907eafa964`, tree
`8c9c588b48bfe0896792b3fd5e45956d2ca5b153`. That source adds the relocatable
OpenSSL build option; application and DSP sources are unchanged. The separately
maintained Linux packaging materials are hashed in the installed inventory.
The matching source companion must include both this runtime source and the exact
packaging materials, plus the pinned external source archives.

Both builds used the architecture-specific Ubuntu image digests in the
[recipe](README.md), GCC/G++ 13.3.0, CMake 3.28.3, glibc 2.39 and Release mode.
Docker Desktop 29.6.1 provided a local Linux/aarch64 engine. amd64 execution used
architecture emulation; arm64 used aarch64 execution in the Linux VM. Separate
build/dependency/staging/output directories were used. Compilation and tests had
networking disabled and no devices passed through. Build processes ran without
root privileges or Linux capabilities, with a read-only container root.

Both enabled static libhackrf 2024.02.1, reviewed static librtlsdr 2.0.3 and shared
libusb 1.0.30. OpenSSL libcrypto 3.5.8 used a fresh relocatable prefix and the
reviewed disabled-loading configuration. SQLite 3.53.4, Nanopb 0.4.9.2, ImGui
1.92.9b, GLFW 3.5.1 and the checked-in DSP/schema sources were included.
CMake found `posix_spawn_file_actions_addclosefrom_np` on both targets, enabling
the separate experimental worker with the reviewed Semtech HAL 2.1.0 subset.

## Results

| Check | amd64 | arm64 |
| --- | --- | --- |
| Dependency hash/header/link/version and AES checks | Passed, including final relocatable crypto build | Passed |
| Initial complete CTest run | 47/51 passed; four filesystem-related failures described below | 51/51 passed on native Linux tmpfs, 34.13 s |
| Unchanged failed-test rerun on native Linux tmpfs | 4/4 passed | Not needed |
| Final crypto-path relink checks | 8/8 passed, 18.76 s | Included in complete final 51-test suite |
| CLI help and bounded synthetic GUI after build | Passed | Passed |
| Package payload, notices, ELF/RUNPATH inspection | Passed | Passed |
| Runtime-only package install, non-root CLI/demo and removal | Passed | Passed |
| Packaged non-root Xvfb/software OpenGL GUI check | Passed | Passed |
| Shared libusb search-path override | Passed | Passed |

The initial amd64 suite ran on a Docker Desktop macOS bind mount. `storage`,
`reports` and `discovery_storage` failed on fixture SQL; `compact_storage` passed
its 27,366 checks but failed cleanup of a shared-memory fixture with a permission
error. The identical four executables passed with fixtures on native Linux tmpfs.
The initial suite must therefore be described as 47 initial passes plus four
successful tmpfs reruns, not an initial 51/51 pass. A first relocation attempt
outside the source hierarchy stopped at the storage test's repository-location
guard before testing its fixtures.

The final amd64 relink ran crypto, protocol, both engine modes and all four storage
tests on Linux tmpfs. Long unchanged discovery tests were not repeated for the
OpenSSL path-only change. Arm64 ran all 51 tests on its final linked build.
No assertions or test cases were disabled. Optional real-time throughput tests
were not enabled; these results do not establish 16 MS/s sustained performance.

Both package GUI checks used Mesa 25.2.8 with llvmpipe/LLVM 20.1.2 and OpenGL 4.5.
The GUI rendered ten synthetic frames with explicit `--ui-smoke 10 --demo`.
The CLI performed a one-second synthetic demo as an unprivileged user. Ordinary
startup, user preferences, radios and GPS were not used. Installation/removal ran
inside disposable containers without privileged mode, networking or host devices;
application launches used an unprivileged account on native Linux filesystems.

The first amd64 runtime-only image lacked `libopengl0`. The package already declared
that dependency, so `dpkg` correctly refused configuration. Adding the declared
distro prerequisite to the test image resolved the failure. Final runtime-only
and GUI-container install/remove checks passed on both architectures.

## Package properties and limits

Each package contains exactly four ELF files: desktop, CLI, worker and shared
libusb. Installed executables use `DT_RUNPATH=$ORIGIN/../lib/ovmeshdrpp`; no absolute
build RPATH or dynamically linked libcrypto/libssl/libhackrf/librtlsdr/SQLite is
required. Payload inspection checks for macOS home paths and dependency-build
paths, excludes operational data and developer diagnostics, and verifies notices.
Full installed-file inventories and exact distro package versions are retained
with the build evidence. Each package contains a public component/source inventory.

Both targets require GLIBC symbols through 2.38 and GLIBCXX symbols through 3.4.32.
Package dependencies include `libc6 (>= 2.38)`, `libstdc++6 (>= 13.1)`, `libopengl0`,
`libgl1` and the documented X11 libraries. The libgcc minimum is 4.0 on amd64 and
4.2 on arm64. Ubuntu 24.04 is the tested distribution; these symbol floors do not
qualify other distributions. Distro C/C++ and graphics libraries are prerequisites,
not copied into the package. Python, CMake and Docker are not runtime dependencies.

The libusb override check copied the packaged library into an alternate directory
and confirmed that `LD_LIBRARY_PATH` selected it. This verifies the loader override
mechanism; a modified libusb implementation was not independently qualified.
No package hooks change device permissions, drivers or services.

Compiler warnings remain recorded, including conversion, indentation, shadow and
HAL macro/ignored-result warnings. GCC's amd64 `std::sort` array-bounds diagnostic
was reviewed against the bounded collection path; it was not treated as proof of
an invalid access or suppressed. No discovery test failed.

Native Linux desktop integration, GPU drivers, Wayland/XWayland behavior, physical
USB permissions, HackRF/RTL/RAK operation, GPS, RF characterization and endurance
remain unvalidated by this packaging work. Full-range waveform-to-payload dispatch
remains an implementation gap. No package was published or uploaded.
