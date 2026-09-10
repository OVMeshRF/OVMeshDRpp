# Building on Linux

OVMeshDR++ is a C++20 application. It does not need a Python virtual environment or pip packages. The optional dependency-setup helper uses Python 3.9 or newer with its standard library only; Python is not an application runtime dependency.

## Prerequisites

Use your distribution's trusted package manager to provide a C++20 compiler and C development environment, CMake 3.24 or newer, make, Perl 5, Python 3.9+, and pkg-config. Desktop builds also need OpenGL and X11 development files, including RandR, Xinerama, Xcursor and XInput headers. See [GLFW's Linux build prerequisites](https://www.glfw.org/docs/latest/compile_guide.html#compile_deps). Native Wayland is disabled; use X11 or XWayland.

RTL-SDR needs libusb 1.0 development files. Its selected driver sources are included. HackRF additionally needs libhackrf development files. You can build only the RTL adapter with `-DOVMESH_ENABLE_HACKRF=OFF`. The build reports missing receiver dependencies; successfully compiling the UI does not establish that a USB adapter was included.

**Do not replace the operating system's OpenSSL to satisfy this project.** The application uses a separate static libcrypto with a reviewed version and build configuration. A system `openssl version` result describes a command-line tool; it does not establish which development headers/library CMake found or whether the required configuration is present.

## Prepare the local crypto dependency

From the checkout directory, explicitly download and build the pinned source:

```sh
python3 tools/bootstrap_openssl.py --download
```

The helper chooses a native Linux x86_64 or aarch64 target, verifies the archive against the checked-in size and SHA-256, rejects unsafe archive entries, builds the reviewed static configuration, and runs the hardened-header/link and AES known-answer checks. It then stages headers, `libcrypto.a` and the license under `build/deps/openssl-3.5.8-local`. It does not run `sudo`, install packages, change system libraries, access radios or install services. Source and build logs stay under ignored `build/deps/openssl-build-*` directories. Existing destination prefixes are never replaced.

The archive checksum is an integrity check, not independent publisher authentication. The detached upstream signature has not been verified; see [source provenance and remaining distribution checks](../security/openssl-intake.md).

For an offline build, obtain the exact archive identified in [the manifest](../../third_party/openssl-source.json) and use:

```sh
python3 tools/bootstrap_openssl.py --archive /path/to/openssl-3.5.8.tar.gz
```

No download occurs without `--download`. Normal CMake configuration never downloads or builds OpenSSL automatically. Other architectures and cross-compilation require the [manual configuration recipe](../security/openssl-intake.md#build-configuration); they are not qualified by this helper.

## Configure, build and test

Once the local dependency is ready:

```sh
cmake --fresh -S . -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel 4
ctest --test-dir build/native --output-on-failure
./build/native/OVMeshDRpp
```

`--fresh` clears the chosen directory's CMake configuration cache so an earlier system-library selection cannot persist. It does not delete saved surveys. CMake automatically selects the prepared local prefix. For an independently prepared prefix, specify `-DOPENSSL_ROOT_DIR=/absolute/path/to/prefix`; the headers and static library must both belong to that prefix and satisfy the version/configuration checks. Do not point this at the `openssl` executable or an unbuilt source directory.

To make another local crypto build without replacing an existing prefix, use `--prefix build/deps/openssl-another-build` with the helper, then supply its absolute path as `OPENSSL_ROOT_DIR`. Keep prefixes separate when changing architecture or toolchain.

For a CLI-only compile, add `-DOVMESH_BUILD_DESKTOP=OFF`; this removes the desktop OpenGL/X11 build requirement. USB capabilities remain independently selectable. `build/native/ovmesh-cli --help` does not open hardware.

Before starting real reception, close Gqrx or other software using the same dongle, select **RTL-SDR / USB** or **HackRF One / USB**, and review the [receiver limits and USB requirements](hardware-compatibility.md). Ordinary startup keeps RF stopped. Linux device permissions and a conflicting DVB driver are separate from an OpenSSL build error; the application does not change them automatically.

## If configuration still fails

Report the exact error, Linux distribution/version, architecture, compiler and CMake versions, and whether the local setup helper completed. A missing prefix, mismatched version, stale cache, missing hardening, and a missing USB development package have different remedies. Do not send private survey files, keys, serial numbers or GPS information. Use [community support](../../SUPPORT.md).

The setup workflow has local macOS build validation and synthetic error-path tests. Linux compilation, desktop behavior, USB permissions and live hardware acceptance remain pending independent Linux testing. Passing the crypto checks does not qualify the receiver or the entire Linux application.
