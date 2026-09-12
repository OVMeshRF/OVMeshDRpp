# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic USB bootstrap boundary checks; no downloads, source builds, or devices."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch


REPO = Path(__file__).resolve().parents[1]


def load_file(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# Load the sibling explicitly so test imports do not depend on a user's PYTHONPATH.
archives = load_file("usb_test_archive_helpers", REPO / "tools/bootstrap_openssl.py")
with patch.dict(sys.modules, {"bootstrap_openssl": archives}):
    helper = load_file("usb_test_bootstrap", REPO / "tools/bootstrap_usb.py")


class BuildBoundary(RuntimeError):
    """Stop at the first build command; no source code is ever executed."""


class USBBootstrapTests(unittest.TestCase):
    def setUp(self):
        base = REPO / "build/usb-bootstrap-tests"
        base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "third_party").mkdir()
        self.components = [self.archive("libusb", "bz2"), self.archive("hackrf", "xz")]
        self.local = {entry["component"]: self.root / entry["name"] for entry in self.components}
        self.prefix = self.root / "build/deps/reviewed-usb"
        self.write_manifest()
        self.output = io.StringIO()
        self.enterContext(contextlib.redirect_stdout(self.output))
        self.enterContext(patch.object(helper.platform, "system", return_value="Darwin"))
        self.enterContext(patch.object(helper.platform, "machine", return_value="arm64"))
        self.enterContext(patch.object(helper.shutil, "which", return_value="fixture-build-tool"))

    def archive(self, component, compression):
        directory = component + "-fixture"
        name = component + ".tar." + compression
        path = self.root / name
        with tarfile.open(path, "w:" + compression) as archive:
            item = tarfile.TarInfo(directory + "/configure")
            item.mode = 0o755
            item.mtime = 1234567890
            item.size = len(b"never execute fixture\n")
            archive.addfile(item, io.BytesIO(b"never execute fixture\n"))
        return {"component": component, "name": name, "source_directory": directory,
                "bytes": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "source_file_hashes": {"configure": hashlib.sha256(b"never execute fixture\n").hexdigest()}}

    def write_manifest(self):
        (self.root / "third_party/usb-source.json").write_text(json.dumps({"components": self.components}))

    def bootstrap(self, local=None, allow_download=False, prefix=None):
        return helper.bootstrap(self.root, self.local if local is None else local,
                                allow_download, prefix or self.prefix, 1)

    def test_bz2_and_xz_extract_bytes_execute_bit_and_mtime(self):
        for entry in self.components:
            with self.subTest(component=entry["component"]):
                destination = self.root / (entry["component"] + "-output")
                destination.mkdir()
                snapshot = archives.snapshot_archive(self.local[entry["component"]],
                    destination / "verified-archive", entry)
                result = helper.extract_verified(snapshot, destination, entry)
                source = result / "configure"
                self.assertEqual(source.read_bytes(), b"never execute fixture\n")
                self.assertEqual(source.stat().st_mode & 0o777, 0o700)
                self.assertEqual(int(source.stat().st_mtime), 1234567890)

    def test_either_archive_pin_failure_stops_before_build(self):
        for component in ("libusb", "hackrf"):
            with self.subTest(component=component):
                entry = next(item for item in self.components if item["component"] == component)
                original = entry["sha256"]
                entry["sha256"] = "0" * 64
                self.write_manifest()
                with patch.object(helper, "run") as execute, patch.object(archives, "download") as network:
                    with self.assertRaisesRegex(ValueError, "size/SHA-256"):
                        self.bootstrap()
                execute.assert_not_called()
                network.assert_not_called()
                self.assertFalse(self.prefix.exists())
                entry["sha256"] = original

    def test_build_source_pin_failure_stops_before_build(self):
        self.components[1]["source_file_hashes"]["configure"] = "0" * 64
        self.write_manifest()
        with patch.object(helper, "run") as execute:
            with self.assertRaisesRegex(ValueError, "build-source hash mismatch"):
                self.bootstrap()
        execute.assert_not_called()

    def test_private_snapshots_survive_replacement_of_both_input_archives(self):
        original_verify = archives.verify_archive
        def replace_input(snapshot, entry):
            original_verify(snapshot, entry)
            self.assertTrue(snapshot.is_relative_to(self.root / "build/deps"))
            self.local[entry["component"]].write_bytes(b"replaced supplier path")
        def first_build(command, cwd, log, env):
            for entry in self.components:
                self.assertEqual((cwd.parent / entry["source_directory"] / "configure").read_bytes(),
                                 b"never execute fixture\n")
            raise BuildBoundary()
        with patch.object(archives, "verify_archive", side_effect=replace_input), \
                patch.object(helper, "run", side_effect=first_build) as execute:
            with self.assertRaises(BuildBoundary):
                self.bootstrap()
        execute.assert_called_once()

    def test_output_escape_or_symlink_stops_before_intake(self):
        (self.root / "build").mkdir()
        (self.root / "build/link").symlink_to(self.root / "third_party", target_is_directory=True)
        for prefix in (self.root / "outside", self.root / "build", self.root / "build/link/prefix"):
            with self.subTest(prefix=prefix), patch.object(archives, "snapshot_archive") as snapshot, \
                    patch.object(helper, "run") as execute:
                with self.assertRaises(ValueError):
                    self.bootstrap(prefix=prefix)
                snapshot.assert_not_called()
                execute.assert_not_called()

    def test_existing_prefix_stops_before_network_or_execution(self):
        self.prefix.mkdir(parents=True)
        marker = self.prefix / "keep"
        marker.write_text("existing prefix")
        with patch.object(archives, "download") as network, patch.object(helper, "run") as execute:
            with self.assertRaisesRegex(ValueError, "already exists"):
                helper.bootstrap(self.root, None, True, self.prefix, 1)
        network.assert_not_called()
        execute.assert_not_called()
        self.assertEqual(marker.read_text(), "existing prefix")

    def test_download_requires_an_unambiguous_choice(self):
        for local, download in ((None, False), (self.local, True)):
            with self.subTest(download=download), patch.object(archives, "download") as network, \
                    patch.object(helper, "run") as execute:
                with self.assertRaisesRegex(ValueError, "Choose --download"):
                    helper.bootstrap(self.root, local, download, self.prefix, 1)
                network.assert_not_called()
                execute.assert_not_called()

    def test_ambient_make_cmake_and_pkgconfig_hooks_are_not_inherited(self):
        hooks = {"CMAKE_TOOLCHAIN_FILE": "/must-not-import/toolchain.cmake",
                 "MAKEFILES": "/must-not-import/additional.mk",
                 "PKG_CONFIG": "/must-not-run/pkg-config"}
        captured = {}
        def first_build(command, cwd, log, env):
            captured.update(env)
            raise BuildBoundary()
        with patch.dict(os.environ, hooks), patch.object(helper, "run", side_effect=first_build):
            with self.assertRaises(BuildBoundary):
                self.bootstrap()
        for name in hooks:
            self.assertNotIn(name, captured, name + " can override the reviewed build configuration")
        self.assertEqual(captured["CONFIG_SITE"], "/dev/null")
        self.assertTrue(Path(captured["TMPDIR"]).is_relative_to(self.root / "build/deps"))

    def test_receipt_matches_cmake_canonical_file_contract(self):
        prefix = self.root / "receipt-prefix"
        names = ("include/libusb-1.0/libusb.h", "lib/libusb-1.0.0.dylib",
                 "include/libhackrf/hackrf.h", "lib/libhackrf.a")
        for name in names:
            path = prefix / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode())
        (prefix / "lib/libusb-1.0.dylib").symlink_to("libusb-1.0.0.dylib")
        helper.write_receipt(prefix, "libusb-1.0.0.dylib")
        receipt = json.loads((prefix / "ovmesh-usb-intake.json").read_text())
        self.assertEqual(receipt["version"], 1)
        self.assertEqual(receipt["libusb_version"], "1.0.30")
        self.assertEqual(receipt["hackrf_version"], "2024.02.1")
        self.assertEqual(set(receipt["files"]), set(names))
        for name, digest in receipt["files"].items():
            self.assertEqual(digest, hashlib.sha256((prefix / name).read_bytes()).hexdigest())

    def test_generated_symlink_is_not_copied_as_reviewed_library(self):
        source = self.root / "generated-library"
        source.symlink_to(self.local["libusb"])
        with self.assertRaisesRegex(ValueError, "Unexpected generated"):
            helper.copy_regular(source, self.root / "must-not-create")
        self.assertFalse((self.root / "must-not-create").exists())

    def test_cli_rejects_missing_conflicting_or_unbounded_arguments(self):
        for args in ([], ["--download", "--libusb-archive", "unused"],
                     ["--libusb-archive", "unused"], ["--download", "--jobs", "33"]):
            with self.subTest(args=args):
                result = subprocess.run([sys.executable, "-B", str(REPO / "tools/bootstrap_usb.py"), *args],
                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertNotIn("Build work directory:", result.stdout)


if __name__ == "__main__":
    unittest.main()
