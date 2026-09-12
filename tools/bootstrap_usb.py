#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build reviewed USB libraries locally, without opening devices or installing drivers.

Network access requires --download. Ordinary application builds never run this helper.
"""
import argparse
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import subprocess
import tarfile
import tempfile

import bootstrap_openssl as archives

REPO = Path(__file__).resolve().parents[1]


def extract_verified(path, destination, entry):
    # Callers supply only a verified private snapshot or verified private download.
    with tarfile.open(path, "r:*") as archive:
        members = archives.validated_members(archive, entry["source_directory"])
        for member in members:
            target = destination.joinpath(*PurePosixPath(member.name).parts)
            if member.isdir():
                target.mkdir(mode=0o700, parents=True, exist_ok=True)
                continue
            target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            with archive.extractfile(member) as source, target.open("xb") as output:
                shutil.copyfileobj(source, output)
            target.chmod(0o700 if member.mode & 0o111 else 0o600)
            os.utime(target, (member.mtime, member.mtime))
    source = destination / entry["source_directory"]
    for name, expected in entry["source_file_hashes"].items():
        if archives.sha256(source / name) != expected:
            raise ValueError("Reviewed build-source hash mismatch: " + name)
    return source


def run(command, cwd, log, env):
    with log.open("ab") as output:
        output.write(("\n" + repr(command) + "\n").encode())
        output.flush()
        subprocess.run(command, cwd=cwd, env=env, stdout=output,
                       stderr=subprocess.STDOUT, check=True)


def copy_regular(source, destination):
    if source.is_symlink() or not source.is_file():
        raise ValueError("Unexpected generated library/header: " + str(source))
    destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def write_receipt(staged, library_name):
    names = ["include/libusb-1.0/libusb.h", "lib/" + library_name,
             "include/libhackrf/hackrf.h", "lib/libhackrf.a"]
    receipt = {"version": 1, "libusb_version": "1.0.30", "hackrf_version": "2024.02.1",
               "files": {name: archives.sha256(staged / name) for name in names}}
    (staged / "ovmesh-usb-intake.json").write_text(json.dumps(receipt, indent=2) + "\n")


def bootstrap(repo, local_archives, allow_download, prefix, jobs):
    system = platform.system()
    if system not in ("Darwin", "Linux") or platform.machine().lower() not in (
            "arm64", "aarch64", "x86_64"):
        raise ValueError("This helper supports native macOS/Linux arm64/x86_64. "
                         "Windows intake and packaging are not yet qualified.")
    prefix = archives.checked_output_path(repo, prefix)
    if prefix.exists():
        raise ValueError("Destination already exists and was left unchanged; choose a new --prefix.")
    if bool(allow_download) == bool(local_archives):
        raise ValueError("Choose --download or both local archive paths, not both.")
    for tool in ("cc", "make", "cmake"):
        if not shutil.which(tool):
            raise ValueError("Missing build tool: " + tool)
    entries = json.loads((repo / "third_party/usb-source.json").read_text())["components"]
    parent = archives.checked_output_path(repo, repo / "build/deps")
    parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="usb-build-", dir=parent))
    log = work / "build.log"
    print("Build work directory:", work, flush=True)
    sources = {}
    for entry in entries:
        destination = work / entry["name"]
        if allow_download:
            print("Downloading and verifying", entry["name"], flush=True)
            archives.download(entry, destination)
        else:
            archives.snapshot_archive(local_archives[entry["component"]], destination, entry)
        sources[entry["component"]] = extract_verified(destination, work, entry)

    # Do not inherit compiler/linker or pkg-config injection into the intake.
    env = os.environ.copy()
    for name in ("CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "LIBS",
                 "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH",
                 "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "DYLD_INSERT_LIBRARIES",
                 "LD_LIBRARY_PATH", "LD_PRELOAD", "CONFIG_SITE", "MAKEFLAGS", "MFLAGS",
                 "CMAKE_PREFIX_PATH", "CMAKE_TOOLCHAIN_FILE", "MAKEFILES",
                 "AR", "AS", "LD", "NM", "RANLIB", "STRIP", "ARFLAGS",
                 "PKG_CONFIG", "PKG_CONFIG_SYSROOT_DIR", "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR"):
        env.pop(name, None)
    (work / "tmp").mkdir()
    env.update({"TMPDIR": str(work / "tmp"), "CC": shutil.which("cc"),
                "CONFIG_SITE": "/dev/null", "PKG_CONFIG_LIBDIR": str(work / "empty-pkgconfig")})
    usb = sources["libusb"]
    arguments = ["--prefix=" + str(prefix), "--enable-shared", "--disable-static",
                 "--disable-examples-build", "--disable-tests-build", "--disable-system-log"]
    if system == "Linux":
        arguments.append("--disable-udev")
    print("Building shared libusb 1.0.30; details in", log, flush=True)
    run([str(usb / "configure"), *arguments], usb, log, env)
    run(["make", "-C", "libusb", "-j", str(jobs)], usb, log, env)
    staged = work / "staged"
    (staged / "lib").mkdir(parents=True)
    if system == "Darwin":
        library_name = "libusb-1.0.0.dylib"
        aliases = ["libusb-1.0.dylib"]
    else:
        candidates = [p for p in (usb / "libusb/.libs").glob("libusb-1.0.so.*")
                      if not p.is_symlink() and p.is_file()]
        if len(candidates) != 1:
            raise ValueError("Expected exactly one versioned shared libusb build output.")
        library_name = candidates[0].name
        aliases = ["libusb-1.0.so", "libusb-1.0.so.0"]
    library = staged / "lib" / library_name
    copy_regular(usb / "libusb/.libs" / library_name, library)
    for alias in aliases:
        (staged / "lib" / alias).symlink_to(library_name)
    copy_regular(usb / "libusb/libusb.h", staged / "include/libusb-1.0/libusb.h")
    copy_regular(usb / "COPYING", staged / "licenses/libusb-COPYING.txt")

    hackrf = sources["hackrf"]
    hackrf_build = work / "hackrf-build"
    print("Building static HackRF against the reviewed USB headers...", flush=True)
    run(["cmake", "-G", "Unix Makefiles", "-S", str(hackrf / "host/libhackrf"),
         "-B", str(hackrf_build), "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
         "-DCMAKE_BUILD_TYPE=Release", "-DRELEASE=2024.02.1", "-DINSTALL_UDEV_RULES=OFF",
         "-DLIBUSB_INCLUDE_DIR=" + str(staged / "include/libusb-1.0"),
         "-DLIBUSB_LIBRARIES=" + str(library)], work, log, env)
    run(["cmake", "--build", str(hackrf_build), "--target", "hackrf-static",
         "--parallel", str(jobs)], work, log, env)
    copy_regular(hackrf_build / "src/libhackrf.a", staged / "lib/libhackrf.a")
    copy_regular(hackrf / "host/libhackrf/src/hackrf.h", staged / "include/libhackrf/hackrf.h")
    copy_regular(repo / "third_party/libhackrf-NOTICE.txt", staged / "licenses/libhackrf-NOTICE.txt")
    write_receipt(staged, library_name)

    # Validate the same fail-closed CMake module used by the application. Only
    # library version accessors run; neither library is initialized.
    probe = work / "probe"
    probe.mkdir()
    (probe / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.24)\nproject(USBIntake C)\n'
        'find_package(Threads REQUIRED)\n'
        'set(OVMESH_ENABLE_HACKRF ON)\nset(OVMESH_ENABLE_RTLSDR ON)\n'
        'include("${OVMESH_SOURCE_DIR}/cmake/ReviewedUSB.cmake")\n')
    probe_env = env.copy()
    if system == "Darwin":
        # Install-name already points to the final prefix. Use only this private
        # staging directory for the pre-promotion version probe, without editing
        # or re-signing the library. The final app configure uses no override.
        probe_env["DYLD_LIBRARY_PATH"] = str(staged / "lib")
    print("Checking library hashes, linking and runtime version without USB access...", flush=True)
    run(["cmake", "-G", "Unix Makefiles", "-S", str(probe), "-B", str(probe / "build"),
         "-DOVMESH_SOURCE_DIR=" + str(repo), "-DOVMESH_USB_ROOT_DIR=" + str(staged)], work, log, probe_env)
    prefix = archives.checked_output_path(repo, prefix)
    if prefix.exists():
        raise ValueError("Destination appeared during the build; it was left unchanged.")
    prefix.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    staged.rename(prefix)
    print("Reviewed local USB libraries ready:", prefix, flush=True)
    print("No USB devices, firmware, system packages, services or permission rules were changed.", flush=True)
    return prefix


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--download", action="store_true")
    parser.add_argument("--libusb-archive", type=Path)
    parser.add_argument("--hackrf-archive", type=Path)
    parser.add_argument("--prefix", type=Path, default=REPO / "build/deps/usb-1.0.30-local")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        parser.error("--jobs must be between 1 and 32")
    if args.download and (args.libusb_archive or args.hackrf_archive):
        parser.error("Use --download or both local archive paths, not both")
    if not args.download and not (args.libusb_archive and args.hackrf_archive):
        parser.error("Provide --download or both --libusb-archive and --hackrf-archive")
    local = None if args.download else {"libusb": args.libusb_archive, "hackrf": args.hackrf_archive}
    try:
        bootstrap(REPO, local, args.download, args.prefix, args.jobs)
    except (OSError, ValueError, tarfile.TarError, subprocess.CalledProcessError) as error:
        parser.exit(1, "USB setup failed: " + str(error) +
                    "\nExisting prefixes are unchanged. Details remain in build/deps/usb-build-*/build.log.\n")


if __name__ == "__main__":
    main()
