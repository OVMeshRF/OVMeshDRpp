# Building on Linux

OVMeshDR++ is a C++20 application. It does not need a Python virtual environment or pip packages. The explicit dependency-setup helpers use Python 3.9 or newer with its standard library only; Python is not an application runtime dependency.

## Prerequisites

Use your distribution's trusted package manager to provide a C++20 compiler and C development environment, CMake 3.24 or newer, make, Perl 5, Python 3.9+, and pkg-config. Desktop builds also need OpenGL and X11 development files, including RandR, Xinerama, Xcursor and XInput headers. See [GLFW's Linux build prerequisites](https://www.glfw.org/docs/latest/compile_guide.html#compile_deps). Native Wayland is disabled; use X11 or XWayland.

HackRF and RTL-SDR require the reviewed local USB prefix prepared below: shared libusb 1.0.30 and, for HackRF, static libhackrf 2024.02.1. RTL-SDR's selected driver sources are included. CMake does not fall back to distribution-installed USB libraries. To build the RTL adapter without the other hardware adapters, use `-DOVMESH_ENABLE_HACKRF=OFF -DOVMESH_ENABLE_RAK5146=OFF`. Disabling both `OVMESH_ENABLE_HACKRF` and `OVMESH_ENABLE_RTLSDR` removes the libusb/libhackrf requirement; RAK remains a separate choice.

Experimental RAK5146 USB/LBT support builds the included minimal Semtech HAL into a separate local worker. It uses USB CDC serial access and does not require a packet forwarder or additional Python package. Linux must provide the child-descriptor isolation checked by CMake (available in glibc 2.34+); otherwise the RAK worker is disabled while other adapters remain available. Use `-DOVMESH_ENABLE_RAK5146=OFF` to omit it. Keep `ovmesh-rak-worker` beside the application executable. See [RAK setup](rak5146.md); Linux live concentrator operation is not yet qualified.

**Do not replace the operating system's OpenSSL to satisfy this project.** The application uses a separate static libcrypto with a reviewed version and build configuration. A system `openssl version` result describes a command-line tool; it does not establish which development headers/library CMake found or whether the required configuration is present.

## Prepare the local crypto dependency

Perl must include `Time::Piece`; the helper checks this before downloading or building. Some distributions split it into a separate package. On Fedora, install `perl-Time-Piece` through the distribution package manager. This is a build prerequisite, not an application dependency.

From the checkout directory, explicitly download and build the pinned source:

```sh
python3 tools/bootstrap_openssl.py --download
```

The helper chooses a native Linux x86_64 or aarch64 target, verifies the archive against the checked-in size and SHA-256, rejects unsafe archive entries, builds the reviewed static configuration, and runs the hardened-header/link and AES known-answer checks. It then stages headers, `libcrypto.a` and the license under `build/deps/openssl-3.5.8-local`. It does not run `sudo`, install packages, change system libraries, access radios or install services. Source and build logs stay under ignored `build/deps/openssl-build-*` directories. Existing destination prefixes are never replaced.

The helper verifies the pinned archive checksum. The pinned release also has a separately verified detached upstream signature; see [source provenance and remaining distribution checks](../security/openssl-intake.md).

For an offline build, obtain the exact archive identified in [the manifest](../../third_party/openssl-source.json) and use:

```sh
python3 tools/bootstrap_openssl.py --archive /path/to/openssl-3.5.8.tar.gz
```

No download occurs without `--download`. Normal CMake configuration never downloads or builds OpenSSL automatically. Other architectures and cross-compilation require the [manual configuration recipe](../security/openssl-intake.md#build-configuration); they are not qualified by this helper.

## Prepare the local USB dependencies

Use the same native C/CMake/make toolchain to prepare the reviewed libraries:

```sh
python3 tools/bootstrap_usb.py --download
```

For offline preparation, provide both pinned release archives:

```sh
python3 tools/bootstrap_usb.py --libusb-archive /path/to/libusb-1.0.30.tar.bz2 \
  --hackrf-archive /path/to/hackrf-2024.02.1.tar.xz
```

The helper prepares `build/deps/usb-1.0.30-local`; it does not replace system libraries or install device rules. Linux libusb uses `--disable-udev` and its netlink backend, so this USB build does not require libudev development files. Existing USB access permissions and RTL DVB-driver conflicts still require the distribution's documented setup. No download occurs without `--download`, and ordinary CMake configuration does not invoke the helper. See [USB source provenance and validation limits](../security/usb-intake.md).

## Configure, build and test

Once both local dependency prefixes are ready:

```sh
cmake --fresh -S . -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel 4
ctest --test-dir build/native --output-on-failure
./build/native/OVMeshDRpp
```

`--fresh` clears the chosen directory's CMake configuration cache so an earlier system-library selection cannot persist. It does not delete saved surveys. CMake automatically selects the prepared local prefix. For an independently prepared prefix, specify `-DOPENSSL_ROOT_DIR=/absolute/path/to/prefix`; the headers and static library must both belong to that prefix and satisfy the version/configuration checks. Do not point this at the `openssl` executable or an unbuilt source directory.

To make another local crypto build without replacing an existing prefix, use `--prefix build/deps/openssl-another-build` with the helper, then supply its absolute path as `OPENSSL_ROOT_DIR`. Keep prefixes separate when changing architecture or toolchain.

For a separately prepared reviewed USB prefix, pass `-DOVMESH_USB_ROOT_DIR=/absolute/path/to/prefix`. CMake checks the selected shared libusb and static libhackrf inputs together and rejects mixed paths or stale host-library cache entries. The native probe reads library versions without initializing or opening USB. Use `--fresh` or a new build directory when changing the prefix.

For a CLI-only compile, add `-DOVMESH_BUILD_DESKTOP=OFF`; this removes the desktop OpenGL/X11 build requirement. USB capabilities remain independently selectable. `build/native/ovmesh-cli --help` does not open hardware.

Before starting real reception, close other software using the selected device and review the [receiver limits and USB requirements](hardware-compatibility.md). For an SDR, select **RTL-SDR / USB** or **HackRF One / USB**. For concentrators, select **RAK5146 USB/LBT**, then explicitly choose one or two known USB candidates in Settings. Ordinary startup keeps RF stopped. Linux serial/USB permissions and a conflicting RTL DVB driver are separate from an OpenSSL build error; the application does not change them automatically. Use the distribution's documented device-access policy, not a root application launch or blanket device permissions.

## If configuration still fails

An older setup could report that `OpenSSL::Crypto` refers to missing `ZLIB::ZLIB`, even after successfully compiling OpenSSL. CMake was importing unrelated system `openssl.pc` dependencies into the local `no-zlib` archive. Update the checkout; the helper and application now share a prefix-only crypto target. Installing Zlib is not required for this configuration.

If that failure happened during the final probe, the compiled headers and library remain in the reported work directory's `staged` folder. After updating the checkout, they can be reused without recompiling OpenSSL:

```sh
cmake --fresh -S . -B build/native -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR="$PWD/build/deps/openssl-build-REPLACE_WITH_WORK_ID/staged"
cmake --build build/native --parallel 4
ctest --test-dir build/native --output-on-failure
```

Replace the work-directory placeholder with the directory reported by the helper. Configuration checks its headers and linkage; CTest must still pass the runtime crypto checks before using the build.

For a missing or mixed USB-prefix error, prepare the reviewed USB libraries, then configure afresh. Do not bypass the pin by supplying a host `RTLUSB_LIBRARY` or `HACKRF_LIBRARY`. A disabled adapter should be selected explicitly with its CMake option.

Report the exact error, Linux distribution/version, architecture, compiler and CMake versions, and which local setup helpers completed. A missing prefix, mismatched version, stale cache and missing hardening have different remedies. Do not send private survey files, keys, serial numbers or GPS information. Use [community support](../../SUPPORT.md).

The OpenSSL setup workflow has earlier local macOS build validation and synthetic error-path tests. The updated USB dependency build has separate macOS ARM64 Release-build, setup-test and parser-regression results in the [USB validation record](../security/usb-intake.md#validation-and-limits). Ubuntu 24.04 amd64/arm64 container builds, synthetic tests and experimental `.deb` inspection are recorded in [Linux package validation](../../packaging/linux/VALIDATION.md); see the [package recipe and installation instructions](../../packaging/linux/README.md). Native desktop behavior, USB permissions and live hardware acceptance remain pending independent Linux testing. Passing crypto or USB version checks does not qualify the receiver or the entire Linux application.

## Synthetic engine tests and throughput

`engine` and `engine_detailed` exercise the same 16 MS/s synthetic waveform in compact and detailed recording modes. They now wait for processing capacity rather than dropping fixture samples when the host falls behind. They still require a valid decoded fixture, zero dropped samples, complete FFT accounting, saved-data consistency and correct session lifecycle. Their sample timestamps describe RF time, not a real-time performance guarantee. No SDR is opened by these tests.

The optional real-time checks retain the original host-speed requirement. Enable them with `-DOVMESH_ENGINE_THROUGHPUT_TESTS=ON`, rebuild, and run `ctest --test-dir build/native -L performance --output-on-failure`. They run serially and must not be presented as correctness passes if they drop samples or fail to decode. A processing load above 1 with sample drops means the tested workload exceeded available processing capacity; check Release configuration and other CPU load. The 16 MS/s synthetic workload is separate from the RTL-SDR's supported 1/2 MS/s input. USB and discovery coverage must still be checked during actual operation.
