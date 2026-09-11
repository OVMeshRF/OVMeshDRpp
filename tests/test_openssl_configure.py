#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline crypto-prefix/configure regression tests; no radio or network access.

Run with python3 tests/test_openssl_configure.py. Complete header/link tests use
build/deps/openssl-3.5.8-local, or OVMESH_TEST_OPENSSL_PREFIX when provided. They
are explicitly skipped when that reviewed native prefix has not been built.
Fixtures and compiler outputs stay in the repository's ignored build directory.
"""
import os
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]
MODULE = REPO / "cmake" / "ReviewedOpenSSL.cmake"
PREFIX = Path(os.environ.get("OVMESH_TEST_OPENSSL_PREFIX",
    str(REPO / "build/deps/openssl-3.5.8-local"))).resolve()
spec = importlib.util.spec_from_file_location("bootstrap_openssl", REPO / "tools/bootstrap_openssl.py")
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


class OpenSSLConfigureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cmake"):
            raise unittest.SkipTest("CMake is not installed")
        cls.fixture_root = REPO / "build" / "openssl-configure-tests"
        cls.fixture_root.mkdir(parents=True, exist_ok=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=self.fixture_root)
        self.root = Path(self.temp.name).resolve()
        self.source = self.root / "source"
        self.binary = self.root / "binary"
        self.source.mkdir()
        (self.source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(CryptoConfigureFixture LANGUAGES C)\n"
            'option(BUILD_TESTING "Exercise no-tests mode" OFF)\n'
            "find_package(Threads REQUIRED)\n"
            f"include([[{MODULE.as_posix()}]])\n", encoding="utf-8")
        self.environment = os.environ.copy()
        # Test commands must not inherit a user's selection of another prefix.
        self.environment.pop("OPENSSL_ROOT_DIR", None)

    def tearDown(self):
        self.temp.cleanup()

    def configure(self, *args):
        return subprocess.run(["cmake", "-S", str(self.source), "-B",
            str(self.binary), "-DBUILD_TESTING=OFF", *map(str, args)],
            env=self.environment, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=120)

    def assert_failure(self, result, message):
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(message, " ".join(result.stdout.split()))
        self.assertIn("python3 tools/bootstrap_openssl.py --download", result.stdout)

    def copy_prefix(self, destination=None):
        if not (PREFIX / "lib/libcrypto.a").is_file():
            self.skipTest("Build the reviewed native OpenSSL prefix to run header/link tests")
        destination = destination or (self.root / "reviewed")
        destination.mkdir(parents=True)
        shutil.copytree(PREFIX / "include", destination / "include")
        (destination / "lib").mkdir()
        # A hard link avoids duplicating the immutable archive; no fixture ever
        # writes to it. Headers are independent copies for negative checks.
        try:
            os.link(PREFIX / "lib/libcrypto.a", destination / "lib/libcrypto.a")
        except OSError:
            shutil.copy2(PREFIX / "lib/libcrypto.a", destination / "lib/libcrypto.a")
        return destination

    def test_missing_default_prefix_does_not_use_system_openssl(self):
        self.assert_failure(self.configure(), "prefix is missing")

    def test_nonexistent_explicit_prefix_does_not_fall_back(self):
        self.assert_failure(self.configure(f"-DOPENSSL_ROOT_DIR={self.root / 'missing'}"),
            "prefix is missing")

    def test_empty_prefix_is_not_a_system_search_hint(self):
        prefix = self.root / "empty"
        prefix.mkdir()
        self.assert_failure(self.configure(f"-DOPENSSL_ROOT_DIR={prefix}"),
            "header crypto.h is missing")

    def test_missing_archive_cannot_fall_back_to_system_library(self):
        prefix = self.copy_prefix()
        (prefix / "lib/libcrypto.a").unlink()
        self.assert_failure(self.configure(f"-DOPENSSL_ROOT_DIR={prefix}"),
            "static libcrypto archive is missing")

    def test_hardened_prefix_with_tests_disabled(self):
        prefix = self.copy_prefix()
        result = self.configure(f"-DOPENSSL_ROOT_DIR={prefix}")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("configuration and link check passed", result.stdout)
        self.assertIn("BUILD_TESTING:BOOL=OFF", (self.binary / "CMakeCache.txt").read_text())

    def test_default_prefix_is_selected_without_manual_path(self):
        self.copy_prefix(self.source / "build/deps/openssl-3.5.8-local")
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_stock_configuration_is_rejected_with_tests_disabled(self):
        prefix = self.copy_prefix()
        config = prefix / "include/openssl/configuration.h"
        with config.open("a", encoding="utf-8") as stream:
            for flag in ("AUTOLOAD_CONFIG", "DSO", "ENGINE", "SOCK", "HTTP"):
                stream.write(f"\n#undef OPENSSL_NO_{flag}\n")
        self.assert_failure(self.configure(f"-DOPENSSL_ROOT_DIR={prefix}"),
            "configuration or link check failed")

    def test_wrong_patch_version_is_rejected(self):
        prefix = self.copy_prefix()
        version = prefix / "include/openssl/opensslv.h"
        version.write_text(version.read_text().replace("3.5.8", "3.5.7")
            .replace("OPENSSL_VERSION_PATCH  8", "OPENSSL_VERSION_PATCH  7"))
        self.assert_failure(self.configure(f"-DOPENSSL_ROOT_DIR={prefix}"),
            "do not match that version")

    def test_malformed_headers_are_rejected(self):
        prefix = self.copy_prefix()
        (prefix / "include/openssl/crypto.h").write_text("#error malformed fixture header\n")
        self.assert_failure(self.configure(f"-DOPENSSL_ROOT_DIR={prefix}"),
            "configuration or link check failed")

    def test_explicit_headers_outside_prefix_are_rejected(self):
        prefix = self.copy_prefix()
        self.assert_failure(self.configure(f"-DOPENSSL_ROOT_DIR={prefix}",
            f"-DOPENSSL_INCLUDE_DIR={PREFIX / 'include'}"), "Mixed OpenSSL configuration")

    def test_explicit_archive_outside_prefix_is_rejected(self):
        prefix = self.copy_prefix()
        self.assert_failure(self.configure(f"-DOPENSSL_ROOT_DIR={prefix}",
            f"-DOPENSSL_CRYPTO_LIBRARY={PREFIX / 'lib/libcrypto.a'}"),
            "Mixed OpenSSL configuration")

    def test_dynamic_library_override_is_rejected(self):
        prefix = self.copy_prefix()
        dynamic = prefix / "lib/libcrypto.so"
        dynamic.write_bytes(b"not a static archive")
        self.assert_failure(self.configure(f"-DOPENSSL_ROOT_DIR={prefix}",
            f"-DOPENSSL_CRYPTO_LIBRARY={dynamic}"), "requires a static crypto archive")

    def test_changed_prefix_with_stale_cached_paths_is_rejected(self):
        first = self.copy_prefix(self.root / "first")
        second = self.copy_prefix(self.root / "second")
        initial = self.configure(f"-DOPENSSL_ROOT_DIR={first}")
        self.assertEqual(initial.returncode, 0, initial.stdout)
        self.assert_failure(self.configure(f"-DOPENSSL_ROOT_DIR={second}"),
            "Mixed OpenSSL configuration")

    def test_reconfigure_rechecks_modified_headers(self):
        prefix = self.copy_prefix()
        initial = self.configure(f"-DOPENSSL_ROOT_DIR={prefix}")
        self.assertEqual(initial.returncode, 0, initial.stdout)
        config = prefix / "include/openssl/configuration.h"
        with config.open("a", encoding="utf-8") as stream:
            stream.write("\n#undef OPENSSL_NO_AUTOLOAD_CONFIG\n")
        self.assert_failure(self.configure(f"-DOPENSSL_ROOT_DIR={prefix}"),
            "configuration or link check failed")

    def system_crypto_metadata(self, prefix):
        if not shutil.which("pkg-config"):
            self.skipTest("pkg-config is needed to reproduce unrelated system metadata")
        pc = self.root / "pkgconfig"
        pc.mkdir()
        (pc / "openssl.pc").write_text(
            f"prefix={prefix}\nName: openssl\nDescription: unrelated system fixture\n"
            "Version: 3.5.8\nLibs: -L${prefix}/lib -lssl -lcrypto\n"
            "Libs.private: -lz -pthread\nCflags: -I${prefix}/include\n")
        self.environment["PKG_CONFIG_LIBDIR"] = str(pc)
        self.environment["PKG_CONFIG_PATH"] = str(pc)

    def test_system_pkgconfig_zlib_does_not_contaminate_local_crypto(self):
        prefix = self.copy_prefix()
        self.system_crypto_metadata(prefix)
        with (self.source / "CMakeLists.txt").open("a") as output:
            output.write('find_package(PkgConfig REQUIRED)\n'
                'pkg_check_modules(CONTROL REQUIRED openssl)\n'
                'if(NOT "z" IN_LIST CONTROL_STATIC_LIBRARIES)\n'
                '  message(FATAL_ERROR "fixture did not expose its Zlib dependency")\n'
                'endif()\n'
                'get_target_property(deps OpenSSL::Crypto INTERFACE_LINK_LIBRARIES)\n'
                'if("ZLIB::ZLIB" IN_LIST deps OR "z" IN_LIST deps)\n'
                '  message(FATAL_ERROR "unrelated Zlib leaked into local crypto")\n'
                'endif()\n')
        result = self.configure(f"-DOPENSSL_ROOT_DIR={prefix}",
            "-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE")
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_generated_bootstrap_probe_ignores_system_crypto_metadata(self):
        prefix = self.copy_prefix()
        self.system_crypto_metadata(prefix)
        probe = self.root / "probe"
        bootstrap.write_probe(REPO, probe)
        result = subprocess.run(["cmake", "-G", "Unix Makefiles", "-S", str(probe),
            "-B", str(self.binary), f"-DOVMESH_SOURCE_DIR={REPO}",
            f"-DOPENSSL_ROOT_DIR={prefix}", "-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE"],
            env=self.environment, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout)
        built = subprocess.run(["cmake", "--build", str(self.binary)], env=self.environment,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        self.assertEqual(built.returncode, 0, built.stdout)
        checked = subprocess.run([str(self.binary / "intake")], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        self.assertEqual(checked.returncode, 0, checked.stdout)
        self.assertIn("vectors passed", checked.stdout)


if __name__ == "__main__":
    unittest.main()
