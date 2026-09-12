#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline USB-prefix regression tests using synthetic libraries only.

Run python3 tests/test_reviewed_usb_configure.py. Fixtures define only version
accessors; they cannot initialize libusb, enumerate devices, or open radios.
Compiler outputs and temporary prefixes remain under the ignored build folder.
"""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]
MODULE = REPO / "cmake/ReviewedUSB.cmake"


class ReviewedUSBConfigureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cmake"):
            raise unittest.SkipTest("CMake is not installed")
        if os.name == "nt":
            raise unittest.SkipTest("Synthetic shared-library fixtures currently cover macOS/Linux")
        base = REPO / "build/usb-configure-tests"
        base.mkdir(parents=True, exist_ok=True)
        cls.suite_temp = tempfile.TemporaryDirectory(dir=base)
        cls.fixture_root = Path(cls.suite_temp.name)
        cls.addClassCleanup(cls.suite_temp.cleanup)
        cls.library = "libusb-1.0.dylib" if platform.system() == "Darwin" else "libusb-1.0.so"
        cls.fixture = cls.build_fixture("current", 30)
        cls.old_fixture = cls.build_fixture("old", 29)

    @classmethod
    def build_fixture(cls, name, micro):
        root = cls.fixture_root / name
        source = root / "source"
        prefix = root / "prefix"
        source.mkdir(parents=True)
        (prefix / "include/libusb-1.0").mkdir(parents=True)
        (prefix / "include/libhackrf").mkdir(parents=True)
        (prefix / "include/libusb-1.0/libusb.h").write_text(
            "#ifndef USB_FIXTURE_H\n#define USB_FIXTURE_H\n"
            "#define LIBUSB_API_VERSION 0x0100010C\n"
            "struct libusb_version { unsigned short major, minor, micro, nano; "
            "const char *rc, *describe; };\n"
            "const struct libusb_version *libusb_get_version(void);\n#endif\n")
        (prefix / "include/libhackrf/hackrf.h").write_text(
            "const char *hackrf_library_version(void);\n")
        (source / "usb.c").write_text(
            '#include "libusb.h"\n'
            f'static const struct libusb_version version = {{1, 0, {micro}, 0, "", "fixture"}};\n'
            "const struct libusb_version *libusb_get_version(void) { return &version; }\n")
        (source / "hackrf.c").write_text(
            '#include "libusb.h"\n'
            'const char *hackrf_library_version(void) { return libusb_get_version() ? "fixture" : 0; }\n')
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(SyntheticUSB LANGUAGES C)\n"
            f'include_directories([[{prefix / "include/libusb-1.0"}]])\n'
            "add_library(usb-1.0 SHARED usb.c)\n"
            "add_library(hackrf STATIC hackrf.c)\n"
            "set_target_properties(usb-1.0 PROPERTIES MACOSX_RPATH TRUE "
            'INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR TRUE)\n'
            f'set_target_properties(usb-1.0 hackrf PROPERTIES LIBRARY_OUTPUT_DIRECTORY [[{prefix / "lib"}]] '
            f'ARCHIVE_OUTPUT_DIRECTORY [[{prefix / "lib"}]])\n')
        for command in (["cmake", "-S", source, "-B", root / "binary"],
                        ["cmake", "--build", root / "binary"]):
            result = subprocess.run(list(map(str, command)), text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
            if result.returncode:
                raise RuntimeError(result.stdout)
        cls.write_receipt(prefix)
        return prefix

    @staticmethod
    def write_receipt(prefix, **overrides):
        files = {path.relative_to(prefix).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                 for subtree in ("include", "lib") for path in (prefix / subtree).rglob("*")
                 if path.is_file() and not path.is_symlink()}
        receipt = {"version": 1, "libusb_version": "1.0.30", "hackrf_version": "2024.02.1", "files": files}
        receipt.update(overrides)
        (prefix / "ovmesh-usb-intake.json").write_text(json.dumps(receipt))

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=self.fixture_root)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.binary = self.root / "binary"
        self.source.mkdir()
        (self.source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(USBConfigureFixture LANGUAGES C)\n"
            'option(OVMESH_ENABLE_HACKRF "Synthetic HackRF adapter" ON)\n'
            'option(OVMESH_ENABLE_RTLSDR "Synthetic USB consumer" ON)\n'
            "find_package(Threads REQUIRED)\n"
            f"include([[{MODULE}]])\n")
        self.environment = os.environ.copy()
        self.environment.pop("OVMESH_USB_ROOT_DIR", None)

    def prefix(self, name="reviewed", fixture=None):
        destination = self.root / name
        shutil.copytree(fixture or self.fixture, destination)
        return destination

    def configure(self, prefix=None, *args):
        command = ["cmake", "-S", str(self.source), "-B", str(self.binary)]
        if prefix is not None:
            command.append(f"-DOVMESH_USB_ROOT_DIR={prefix}")
        command.extend(map(str, args))
        return subprocess.run(command, env=self.environment, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)

    def assert_failure(self, result, message):
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(message, " ".join(result.stdout.split()))
        self.assertIn("python3 tools/bootstrap_usb.py --download", result.stdout)

    def assert_success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_missing_default_prefix_cannot_select_host(self):
        self.assert_failure(self.configure(), "prefix is missing")

    def test_missing_explicit_prefix_cannot_select_host(self):
        self.assert_failure(self.configure(self.root / "absent"), "prefix is missing")

    def test_empty_prefix_does_not_enable_system_search(self):
        prefix = self.root / "empty"
        prefix.mkdir()
        self.assert_failure(self.configure(prefix), "libusb header is missing")

    def test_both_backends_off_need_no_prefix(self):
        self.assert_success(self.configure(self.root / "absent",
            "-DOVMESH_ENABLE_HACKRF=OFF", "-DOVMESH_ENABLE_RTLSDR=OFF"))

    def test_valid_prefix_links_both_consumers_to_one_shared_target(self):
        with (self.source / "CMakeLists.txt").open("a") as stream:
            stream.write(
                'if(NOT OVMESH_RTLUSB_TARGET STREQUAL "OVMesh::USB")\n'
                '  message(FATAL_ERROR "Wrong RTL-SDR target")\nendif()\n'
                'get_target_property(kind OVMesh::USB TYPE)\n'
                'if(NOT kind STREQUAL "SHARED_LIBRARY")\n'
                '  message(FATAL_ERROR "libusb must be shared")\nendif()\n'
                'get_target_property(kind hackrf_external TYPE)\n'
                'get_target_property(deps hackrf_external INTERFACE_LINK_LIBRARIES)\n'
                'if(NOT kind STREQUAL "STATIC_LIBRARY" OR NOT "OVMesh::USB" IN_LIST deps '
                'OR NOT "Threads::Threads" IN_LIST deps)\n'
                '  message(FATAL_ERROR "HackRF must use static adapter and reviewed USB")\nendif()\n')
        result = self.configure(self.prefix())
        self.assert_success(result)
        self.assertIn("runtime libusb 1.0.30 check passed", result.stdout)

    def test_repository_local_default_prefix(self):
        destination = self.source / "build/deps/usb-1.0.30-local"
        shutil.copytree(self.fixture, destination)
        self.assert_success(self.configure())

    def test_rtlsdr_only_does_not_require_hackrf(self):
        prefix = self.prefix()
        (prefix / "lib/libhackrf.a").unlink()
        shutil.rmtree(prefix / "include/libhackrf")
        self.assert_success(self.configure(prefix, "-DOVMESH_ENABLE_HACKRF=OFF"))

    def test_hackrf_only_still_requires_reviewed_usb(self):
        prefix = self.prefix()
        (prefix / "lib" / self.library).unlink()
        self.assert_failure(self.configure(prefix, "-DOVMESH_ENABLE_RTLSDR=OFF"),
            "shared libusb library is missing")

    def test_hackrf_only_is_supported(self):
        self.assert_success(self.configure(self.prefix(), "-DOVMESH_ENABLE_RTLSDR=OFF"))

    def test_shared_hackrf_is_rejected(self):
        prefix = self.prefix()
        self.assert_failure(self.configure(prefix, f"-DHACKRF_LIBRARY={prefix / 'lib' / self.library}"),
            "requires a static HackRF archive")

    def test_thin_hackrf_archive_is_rejected(self):
        prefix = self.prefix()
        (prefix / "lib/libhackrf.a").write_bytes(b"!<thin>\n")
        self.assert_failure(self.configure(prefix), "requires a regular static HackRF archive")

    def test_renamed_shared_hackrf_is_rejected(self):
        prefix = self.prefix()
        shutil.copyfile(prefix / "lib" / self.library, prefix / "lib/libhackrf.a")
        self.assert_failure(self.configure(prefix), "requires a regular static HackRF archive")

    def test_static_libusb_override_is_rejected(self):
        prefix = self.prefix()
        self.assert_failure(self.configure(prefix, f"-DRTLUSB_LIBRARY={prefix / 'lib/libhackrf.a'}"),
            "requires shared libusb")

    def test_renamed_static_libusb_is_rejected(self):
        prefix = self.prefix()
        shutil.copyfile(prefix / "lib/libhackrf.a", prefix / "lib" / self.library)
        self.assert_failure(self.configure(prefix), "requires shared libusb")

    def test_missing_hackrf_archive_cannot_fall_back(self):
        prefix = self.prefix()
        (prefix / "lib/libhackrf.a").unlink()
        self.assert_failure(self.configure(prefix), "static HackRF archive is missing")

    def test_external_header_override_is_rejected(self):
        prefix = self.prefix()
        outside = self.prefix("outside")
        self.assert_failure(self.configure(prefix, f"-DRTLUSB_INCLUDE_DIR={outside / 'include/libusb-1.0'}"),
            "Mixed USB configuration")

    def test_flat_hackrf_include_override_cannot_select_ambient_header(self):
        prefix = self.prefix()
        self.assert_failure(self.configure(prefix, f"-DHACKRF_INCLUDE_DIR={prefix / 'include/libhackrf'}"),
            "HackRF header is missing")

    def test_external_library_override_is_rejected(self):
        prefix = self.prefix()
        outside = self.prefix("outside")
        self.assert_failure(self.configure(prefix, f"-DHACKRF_LIBRARY={outside / 'lib/libhackrf.a'}"),
            "Mixed USB configuration")

    def test_header_symlink_cannot_escape_prefix(self):
        prefix = self.prefix()
        header = prefix / "include/libusb-1.0/libusb.h"
        header.unlink()
        header.symlink_to(self.fixture / "include/libusb-1.0/libusb.h")
        self.assert_failure(self.configure(prefix), "Mixed USB configuration")

    def test_library_symlink_cannot_escape_prefix(self):
        prefix = self.prefix()
        library = prefix / "lib" / self.library
        library.unlink()
        library.symlink_to(self.fixture / "lib" / self.library)
        self.assert_failure(self.configure(prefix), "Mixed USB configuration")

    def test_receipt_symlink_cannot_escape_prefix(self):
        prefix = self.prefix()
        receipt = prefix / "ovmesh-usb-intake.json"
        receipt.unlink()
        receipt.symlink_to(self.fixture / "ovmesh-usb-intake.json")
        self.assert_failure(self.configure(prefix), "Mixed USB configuration")

    def test_internal_library_symlink_uses_canonical_receipt_entry(self):
        prefix = self.prefix()
        library = prefix / "lib" / self.library
        canonical = library.with_name("canonical-" + self.library)
        library.rename(canonical)
        library.symlink_to(canonical.name)
        self.write_receipt(prefix)
        self.assert_success(self.configure(prefix))

    def test_missing_receipt_is_rejected(self):
        prefix = self.prefix()
        (prefix / "ovmesh-usb-intake.json").unlink()
        self.assert_failure(self.configure(prefix), "intake receipt is missing")

    def test_malformed_receipt_is_rejected(self):
        prefix = self.prefix()
        (prefix / "ovmesh-usb-intake.json").write_text("not json")
        self.assert_failure(self.configure(prefix), "receipt requires libusb 1.0.30")

    def test_old_release_receipt_is_rejected(self):
        prefix = self.prefix()
        self.write_receipt(prefix, libusb_version="1.0.29")
        self.assert_failure(self.configure(prefix), "receipt requires libusb 1.0.30")

    def test_unreviewed_hackrf_receipt_is_rejected(self):
        prefix = self.prefix()
        self.write_receipt(prefix, hackrf_version="unreviewed")
        self.assert_failure(self.configure(prefix), "receipt requires HackRF 2024.02.1")

    def test_digest_mismatch_is_rejected(self):
        prefix = self.prefix()
        with (prefix / "include/libusb-1.0/libusb.h").open("a") as stream:
            stream.write("\n/* changed after intake */\n")
        self.assert_failure(self.configure(prefix), "digest mismatch or missing entry")

    def test_correct_api_macro_does_not_substitute_for_release_check(self):
        prefix = self.prefix(fixture=self.old_fixture)
        self.assert_failure(self.configure(prefix, "-DBUILD_TESTING=OFF"), "runtime release check failed")
        self.assertIn("libusb 1.0.29", (self.binary / "CMakeFiles/ovmesh-usb-check.log").read_text())

    def test_reconfigure_rechecks_file_digests(self):
        prefix = self.prefix()
        self.assert_success(self.configure(prefix))
        with (prefix / "include/libusb-1.0/libusb.h").open("a") as stream:
            stream.write("\n/* changed after configuration */\n")
        self.assert_failure(self.configure(prefix), "digest mismatch or missing entry")

    def test_changed_prefix_rejects_stale_cached_paths(self):
        first, second = self.prefix("first"), self.prefix("second")
        self.assert_success(self.configure(first))
        self.assert_failure(self.configure(second), "Mixed USB configuration")

    def test_override_target_cannot_bypass_reviewed_dependency(self):
        self.assert_failure(self.configure(self.prefix(), "-DOVMESH_RTLUSB_TARGET=PkgConfig::RTLUSB"),
            "may not override the reviewed target")

    def test_ambient_prefix_and_pkgconfig_do_not_contaminate_selection(self):
        prefix, outside = self.prefix(), self.prefix("outside")
        self.environment["CMAKE_PREFIX_PATH"] = str(outside)
        self.environment["PKG_CONFIG_LIBDIR"] = str(outside)
        self.environment["PKG_CONFIG_PATH"] = str(outside)
        self.assert_success(self.configure(prefix, f"-DPKG_CONFIG_EXECUTABLE={outside / 'must-not-be-run'}"))

    def test_cross_configuration_reports_pending_runtime_qualification(self):
        result = self.configure(self.prefix(), f"-DCMAKE_SYSTEM_NAME={platform.system()}")
        self.assert_success(result)
        self.assertIn("qualification remains pending", " ".join(result.stdout.split()))


if __name__ == "__main__":
    unittest.main()
