# Local build and packaging

Status: experimental source with platform packaging candidates in preparation; no binary release is published by these instructions. Builds and validation outputs stay in the repository's ignored `build/` directory. Ordinary operator launches use local application-data storage as described below. The source is hosted on [GitHub](https://github.com/OVMeshRF/OVMeshDRpp); the application has no cloud deployment or system service.

## Build

Use a C++20 compiler, CMake 3.24 or newer, and platform development headers. Native Mac builds use Apple Clang/SDK. GLFW, Dear ImGui and SQLite source are vendored and verified by CMake. Prepare the reviewed local OpenSSL prefix with `python3 tools/bootstrap_openssl.py --download` (native Linux/macOS), or use `--archive PATH` for an offline verified archive. This explicit helper uses Python's standard library, not a virtual environment or pip; it leaves system OpenSSL unchanged. See the [Linux build guide](linux-build.md) and [OpenSSL intake](../security/openssl-intake.md). No system package installation is part of configuration.

For HackRF or RTL-SDR support, prepare shared libusb 1.0.30 and static libhackrf 2024.02.1 in a single local prefix. On native macOS/Linux, explicitly download and build the pinned sources from the repository root:

```sh
python3 tools/bootstrap_usb.py --download
```

For an offline build, supply both exact release archives:

```sh
python3 tools/bootstrap_usb.py --libusb-archive /path/to/libusb-1.0.30.tar.bz2 \
  --hackrf-archive /path/to/hackrf-2024.02.1.tar.xz
```

The default prefix is `build/deps/usb-1.0.30-local`. To use another reviewed prefix, pass `-DOVMESH_USB_ROOT_DIR=/absolute/path/to/prefix` to CMake. Both libraries and headers must belong to that prefix. The helper does not download without `--download`, install system libraries or change device permissions. On Linux it builds libusb with `--disable-udev`, using the netlink backend without installing udev rules. See [USB source intake and validation limits](../security/usb-intake.md). The helper targets native macOS/Linux; Windows dependency preparation and packaging remain unqualified.

After preparing the dependencies, configure and build from the repository root:

```sh
cmake --fresh -S . -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel 4
ctest --test-dir build/native --output-on-failure
```

`cmake -P cmake/VerifyVendored.cmake` separately verifies the reviewed vendor inventory offline. CMake never downloads packages or accesses USB devices. With either SDR adapter enabled, it requires the reviewed local USB prefix and rejects missing inputs, mixed paths and host-library fallback. Its native version probe calls library-version accessors without initializing or enumerating USB. HackRF links static libhackrf and shared libusb; the included RTL-SDR driver links the same shared libusb. `-DOVMESH_ENABLE_HACKRF=OFF` or `-DOVMESH_ENABLE_RTLSDR=OFF` omits the respective adapter; disabling both removes this USB-library requirement. RAK5146 remains independent. Use `--fresh` or a new build directory when changing dependency selection. See [receiver and platform compatibility](hardware-compatibility.md) for rates, spans and validation status.

GPS device discovery uses platform facilities already supplied by the operating system: IOKit/CoreFoundation on macOS, SetupAPI on Windows, and local serial/sysfs metadata on Linux. This adds native link inputs, not a third-party package, GPS service, serial-probing process or network dependency. Desktop preferences use owned bounded text handling, existing OpenSSL random generation, and native file/permission APIs.

For ASan/UBSan, configure a separate `build/sanitized` with `-DOVMESH_SANITIZERS=ON -DOVMESH_BUILD_DESKTOP=OFF` and the same reviewed crypto and USB inputs. Sanitizer timing is not a production throughput benchmark.

## Build isolation and format compatibility

Rebuild the chosen build directory after source changes; an existing executable does not update automatically. Use separate checkouts/build directories and isolated preferences when comparing versions. Preserve current work and existing survey files.

New SDR recordings default to Compact schema 6. Select **Detailed** before SDR recording when schema 5 is required. RAK recordings use separate schema 7 regardless of the SDR recording-mode preference. New recordings add validated acquisition tables; older binaries may reject this extension even when they recognize the numeric schema version. Keep a compatible reader and do not remove tables, rewrite existing surveys or downgrade preferences as a rollback shortcut. See [implementation status](../engineering/implementation-status.md) and [UI validation](../engineering/quality-and-validation.md).

## Run locally

The Mac desktop executable is `build/native/OVMeshDRpp.app/Contents/MacOS/OVMeshDRpp`. Start without arguments for the idle operator interface or use `--demo` for synthetic RF. Version 0.4 ordinary launch restores non-secret receiver preferences, opens a fresh empty workspace, and may connect the enabled uniquely recognized or remembered GPS. It does not start RF reception or open a historical survey. Missing or ambiguous GPS selection requires operator choice. The CLI help does not access hardware.

```sh
build/native/ovmesh-cli --headless-demo --seconds 10 --session "$PWD/build/demo.sqlite"
build/native/ovmesh-cli --export "$PWD/build/demo.sqlite" --output "$PWD/build/demo-summary.csv"
```

These examples use synthetic data only. Use new local filenames for each run; existing files are not overwritten. Real data belongs in a private local directory outside the source repository. See the [user guide](../user-guide.md) and [support playbook](support-playbook.md).

### Spectrum-only desktop scope

The desktop disables and hides LoRa waveform discovery, automatic/manual packet decoding, preset/key setup and packet classifications. This gate applies to new and remembered preferences, demos and explicit desktop launch configurations; older enabled settings cannot turn it off. Start a fresh ordinary session, select the receiver/range and use Start/Stop, recording, GPS and spectrum analysis as described in the [user guide](../user-guide.md).

Existing decoder source and explicit CLI/backend diagnostic flags remain solely for internal development and regression work. They are not an alternate supported user feature, and desktop launch flags cannot enable the hidden paths. Their earlier synthetic results do not establish reliable live decoding.

New SDR surveys default to Compact schema 6; Detailed schema 5 preserves finer power history. Both retain fine joint activity, receiver settings, acquisition gaps and optional GPS associations. RAK schema 7 retains sampled RSSI histograms with its separate observation semantics. Existing survey files remain unchanged, including earlier diagnostic metadata. Spectrum event counts are not packet counts; energy envelopes do not identify LoRa bandwidth/SF. See [validation scope](../engineering/quality-and-validation.md) for current limits. The desktop scope change still requires its own acceptance checks.

### Desktop preferences and default survey storage

Version 0.4 defaults recording and automatic GPS on, with remembered operator opt-outs. The desktop always applies spectrum-only reception; old processing/public-key preferences cannot enable decoding. Preference format 8 retains non-secret receiver setup, recording folder/mode, GPS identity/baud, RAK board/scan setup, typography and position display. Each new survey uses a new filename; Stop/Resume continues the current file with separate acquisition intervals. Preferences do not store coordinates, private keys, messages, historical results or export privacy choices. Offset defaults to zero, is bounded to ±100,000 Hz and resets when switching hardware types; it is not a per-device calibration database.

| Platform | Default application directory |
|---|---|
| macOS | `~/Library/Application Support/OVMeshDRpp` |
| Windows | `%LOCALAPPDATA%\OVMeshDRpp` |
| Linux | `$XDG_STATE_HOME/ovmeshdrpp`, or `~/.local/state/ovmeshdrpp` when unset |

The directory contains `preferences.conf` and the default `Surveys` subdirectory. These are plaintext local files, not encrypted storage. Created application directories are owner-only (0700 on POSIX) and preference files are owner-only (0600 on POSIX); Windows uses native owner-only ACLs. Preference format 8 reads versions 1–7, preserves stored settings and supplies current defaults for fields absent from an older file. Old v3/v4 discovery opt-outs remain off. The next settings save writes version 8; this does not migrate survey databases. Older binaries may reject newer preferences, so use an isolated settings directory for rollback. Strings are bounded UTF-8 encoded as hex for serialization, not secrecy. Replacements use an exclusive private temporary file and atomic replacement. Invalid, unsupported, nonprivate or unreadable existing settings produce a visible error and remain intact. An unavailable selected recording folder does not cause a silent fallback to memory-only capture.

For development validation or a repository-local preview, use an explicit isolated directory:

```sh
build/native/OVMeshDRpp.app/Contents/MacOS/OVMeshDRpp --settings-directory "$PWD/build/desktop-preview"
```

`--settings-directory` is accepted only for ordinary desktop startup; it is not combined with demo, smoke, saved-view or managed receiver modes. Those explicit modes remain isolated from operator preferences and require their existing `--session`/GPS arguments when needed. The override does not grant USB permission or authorize operational data in the repository. Preference tests use explicit build-local fixture directories; they do not write real profile settings. Starting a real survey still requires an explicit receiver action. In ordinary hardware-oriented startup, enabled GPS may connect automatically; Start and Connect GPS now also use only the resolved selected device. Managed, demo and passive launches do not automatically open GPS.

Builds and synthetic tests do not open a receiver. Hardware tests require the device operator's authorization and a defined receive-only scope; successful transport checks do not establish RF sensitivity or decoder interoperability.

For an operator-authorized bounded desktop receiver test, the desktop executable accepts `--desktop-receive-hackrf --confirm-radio-access --seconds 180` or `--desktop-receive-rtlsdr --confirm-radio-access --seconds 180` plus the same center, rate, span, `--lane`, receiver-specific gain and `--tuning-offset-hz` options as the corresponding headless receiver. It opens the visible app, starts the explicitly configured receiver, displays a countdown, stops reception at the deadline and closes the window. The receiver-stop deadline is independent of rendering/minimization. Stop remains available; restarting is disabled during this bounded run. This mode must not be invoked as a build or smoke-test shortcut without the particular hardware-test authorization.

For key entry before a timed test, use `--prepare-desktop-hackrf --seconds 180` or `--prepare-desktop-rtlsdr --seconds 180` with the corresponding receiver/profile options. This mode only opens the visible setup interface. Enter authorized channel keys, then use the normal UI consent dialog to start. The receiver-stop timer is armed only after reception starts successfully. Combining this mode with `--confirm-radio-access` is rejected. With `--ui-smoke N`, prepared setup closes after N frames and its Start button is disabled; this passive check cannot open the receiver. Key material is never accepted as command-line argument values or printed in diagnostics.

A controlled local launcher can instead provide one authorized key with `--channel-key-stdin LANE,CHANNEL` (lane numbers 1–4). The argument identifies the profile and exact channel name; the key itself comes from redirected standard input as one line of at most 64 characters. Interactive terminal entry is refused to prevent terminal echo. Conflicting modes, missing direct-start consent and absent lanes are rejected before input is read; invalid key input fails before UI/device access. No key file is created by the application. A launcher must avoid logging its input or placing a private key in shell history, process arguments, or a script. A stalled pipe can delay startup until it supplies a newline or EOF; the radio has not opened while this input is pending.

## Operator-controlled desktop sessions

Use `--until-stopped` in place of `--seconds` to leave reception running until the operator stops it, then keep the window and results open. It is supported with either `--desktop-receive-hackrf` or `--desktop-receive-rtlsdr` plus `--confirm-radio-access`, either `--prepare-desktop-hackrf` or `--prepare-desktop-rtlsdr`, and synthetic `--demo`. It conflicts with any explicit `--seconds`, `--ui-smoke`, or headless mode. The prepared mode still opens no radio before UI consent; the direct hardware mode still requires authorization for that particular run.

For a synthetic check with retained results and no radio access:

```sh
build/native/OVMeshDRpp.app/Contents/MacOS/OVMeshDRpp --demo --until-stopped --session "$PWD/build/demo-until-stopped.sqlite"
```

The session filename must be new. `--session` enables saving before reception; alternatively enable **Save survey measurements** and use **Browse recording location...** to choose a local folder and new filename during prepared setup. **Stop reception** stops acquisition while leaving the window open for review. On macOS/Linux, `SIGINT` to the process ID printed by this launch requests the same stop through the GUI event loop; it is not an independent watchdog. Windows uses **Stop reception**. Closing the window stops reception and exits. This mode has no automatic deadline; existing timed runs continue to stop and close at their original deadline. The managed window keeps its current session selected; exports are available after stopping, and other saved surveys can be opened after a subsequent ordinary desktop launch.

See [validation scope](../engineering/quality-and-validation.md) for software coverage and remaining receiver/decoder qualification.

Keep operational recordings in private local storage outside the source checkout. On POSIX, restrict survey directories to mode 0700 and session files to 0600; Windows uses owner-only ACLs. Do not commit or upload recordings or exports containing private messages, precise positions, keys or device identifiers. Ignore rules do not provide access control.

## Platform and distribution limits

| Candidate | Verified scope | Remaining release limits |
|---|---|---|
| macOS arm64 app and drag-to-Applications DMG | Built for macOS 13.0+, tested on macOS 26.3; native offline tests and passive/synthetic launches from staged and relocated app copies passed. Mach-O dependencies, notices and included source materials checked | Ad-hoc review signatures only; Developer ID signing, notarization and installation acceptance pending. macOS 13 runtime, clean-machine and packaged hardware acceptance are not established. No Intel/Universal build |
| Ubuntu 24.04 amd64 `.deb` | Independently compiled with offline test checks; minimal-runtime and builder-container installation, CLI/synthetic demo, shared-libusb replacement and removal passed; builder Xvfb GUI launch passed | Container results do not qualify a physical desktop, USB permissions, GPS or live receivers. No compatibility claim for other distributions or older Ubuntu versions |
| Ubuntu 24.04 arm64 `.deb` | Independently compiled; full offline test suite, Xvfb GUI launch, minimal-runtime installation, non-root CLI/synthetic demo, shared-libusb replacement and removal passed | Final package/source review remains in progress; the same physical-desktop and hardware limits apply |
| Windows x64 portable ZIP | Dedicated build/dependency preparation underway; intended HackRF/RTL-SDR adapters, with RAK disabled | No successful application build, test suite or ZIP acceptance yet. Driver binding, GPS and live receiver validation remain pending |

The macOS candidate passed 51/51 native CTests. On Ubuntu amd64, the initial run passed 47/51 tests; four storage tests affected by the macOS-hosted bind filesystem passed with the same binaries on native Linux tmpfs. The final crypto relink passed eight focused checks. Ubuntu arm64 passed 51/51 tests on native Linux tmpfs. These are build/fixture results, not RF capture or field-performance evidence.

Follow the [Apple Silicon package recipe](../../packaging/macos/README.md) or [Ubuntu package recipe](../../packaging/linux/README.md). They stage the replaceable shared libusb, required worker, notices and corresponding source/build materials, and check final library paths. Distributable builds use the OpenSSL helper's `--relocatable` option with fresh prefixes to avoid embedding local OpenSSL directory strings; this retains the hardened crypto configuration and does not replace final binary inspection. Ordinary development `.app` output can still reference its build-specific USB prefix and must not be distributed as a finished package.

Linux selects X11/XWayland; native Wayland is disabled. Both Ubuntu candidates require glibc 2.38 and libstdc++ 13.1 or newer, while their tested build/runtime baseline remains Ubuntu 24.04. System display libraries remain distribution dependencies rather than bundled replacements. Windows uses native serial, device-enumeration and file-permission APIs. Each platform still requires its own hardware acceptance. Do not disable Gatekeeper, strip quarantine or change system library paths to bypass package failures.

**About & licenses** embeds the reviewed notice texts into the executable, so a copied `.app` can display them without locating the source checkout. CLI `--licenses` prints the same texts without hardware access. `cmake --install` additionally places `LICENSES.txt`, `NOTICE`, `LICENSE`, the inventory and selected component notices under `share/OVMeshDRpp`. This is notice packaging only; it does not bundle host libraries or supply a complete Corresponding Source release. Use a repository-local staging prefix for development checks. See the [license review and remaining distribution gate](../engineering/dependencies-and-licensing.md#source-attribution-and-distribution-limits).

A supported binary release requires hardware/interoperability/endurance evidence, security/support ownership and maintainer approval. No automatic download or self-replacement runs at startup.

## Retained developer decoder interfaces

The retained CLI key-input and decoder flags are internal development interfaces. They do not enable decoder features in the spectrum-only desktop. Never put key material in argument values or environment variables, and never publish operational diagnostic inputs or recordings. Existing metadata-only retention and hardware-authorization boundaries still apply to development work.

## Source publication

Experimental source is available on [GitHub](https://github.com/OVMeshRF/OVMeshDRpp). Source updates follow the [release checklist](github-publication.md). Review the exact source tree and history being shared; exclude private build folders, preferences, recordings and exports. Source availability does not make the local application bundle a supported redistributable binary.

## RTL-SDR backend

The optional RTL-SDR adapter builds the pinned library sources directly against libusb. It has independent sample-rate/span/gain controls and stores receiver type plus applied manual tuner gain or automatic gain mode. See [setup, limits and USB troubleshooting](rtl-sdr.md). Current preference format 8 preserves the RTL settings introduced in format 6 and reads older settings.

## RAK5146 backend

On macOS and supported Linux builds, `OVMESH_ENABLE_RAK5146=ON` builds the pinned minimal Semtech HAL into `ovmesh-rak-worker`. The main application starts one local worker per selected board; no packet forwarder, network service, system installation or firmware flashing is involved. Linux requires the safe child-descriptor closure checked by CMake (provided by glibc 2.34+). Unsupported platforms disable this adapter while leaving the SDR adapters available.

Keep `ovmesh-rak-worker` beside `ovmesh-cli`, or inside the desktop bundle's executable directory. The normal build/install targets arrange this; copying only the main executable omits the adapter. Use `-DOVMESH_ENABLE_RAK5146=OFF` to omit the worker. See [RAK setup and validation limits](rak5146.md) and [source provenance](../../third_party/sx1302_hal/PROVENANCE.md). Windows RAK operation and Linux live USB qualification are not claimed.
