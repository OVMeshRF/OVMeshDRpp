# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic archive/output-boundary checks; no downloads or source execution."""
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("bootstrap_openssl",
    Path(__file__).resolve().parents[1] / "tools/bootstrap_openssl.py")
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name).resolve()

    def tearDown(self):
        self.temp.cleanup()

    def archive(self, entries):
        path = self.root / "fixture.tar.gz"
        with tarfile.open(path, "w:gz") as archive:
            for name, kind in entries:
                item = tarfile.TarInfo(name)
                item.type = kind
                if kind == tarfile.REGTYPE:
                    item.size = 4
                    archive.addfile(item, io.BytesIO(b"test"))
                else:
                    item.linkname = "../../outside"
                    archive.addfile(item)
        return path

    def test_archive_integrity_checks_bytes_and_hash(self):
        path = self.archive([("openssl-test/a", tarfile.REGTYPE)])
        good = {"bytes": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        helper.verify_archive(path, good)
        with self.assertRaises(ValueError):
            helper.verify_archive(path, {**good, "bytes": good["bytes"] + 1})
        with self.assertRaises(ValueError):
            helper.verify_archive(path, {**good, "sha256": "0" * 64})

    def test_extract_preserves_regular_bytes_only(self):
        path = self.archive([("openssl-test", tarfile.DIRTYPE), ("openssl-test/a", tarfile.REGTYPE)])
        out = self.root / "out"
        out.mkdir()
        result = helper.extract_verified(path, out, "openssl-test")
        self.assertEqual((result / "a").read_bytes(), b"test")

    def test_private_snapshot_survives_source_replacement(self):
        path = self.archive([("openssl-test/a", tarfile.REGTYPE)])
        original = path.read_bytes()
        entry = {"bytes": len(original), "sha256": hashlib.sha256(original).hexdigest()}
        copied = helper.snapshot_archive(path, self.root / "private.tar.gz", entry)
        path.write_bytes(b"replaced after verification")
        self.assertEqual(copied.read_bytes(), original)
        self.assertEqual(copied.stat().st_mode & 0o777, 0o600)
        out = self.root / "out"
        out.mkdir()
        self.assertEqual((helper.extract_verified(copied, out, "openssl-test") / "a").read_bytes(), b"test")

    def test_snapshot_rejects_wrong_content_size_and_nonregular_input(self):
        path = self.archive([("openssl-test/a", tarfile.REGTYPE)])
        entry = {"bytes": path.stat().st_size, "sha256": "0" * 64}
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            helper.snapshot_archive(path, self.root / "bad-hash", entry)
        with self.assertRaisesRegex(ValueError, "reviewed size"):
            helper.snapshot_archive(path, self.root / "bad-size", {**entry, "bytes": 1})
        if hasattr(os, "mkfifo"):
            fifo = self.root / "fifo"
            os.mkfifo(fifo)
            with self.assertRaisesRegex(ValueError, "regular file"):
                helper.snapshot_archive(fifo, self.root / "bad-type", entry)

    def test_bootstrap_uses_private_snapshot_after_verification(self):
        path = self.archive([("openssl-test/a", tarfile.REGTYPE)])
        original = path.read_bytes()
        (self.root / "third_party").mkdir()
        (self.root / "third_party/openssl-source.json").write_text(json.dumps({
            "version": "test", "files": [{"name": "fixture.tar.gz", "bytes": len(original),
                "sha256": hashlib.sha256(original).hexdigest()}],
            "source_file_hashes": {"a": hashlib.sha256(b"test").hexdigest()},
            "build_configuration": {"configure_arguments": [], "targets": []}}))
        verify = helper.verify_archive
        def replace_original(checked, entry):
            verify(checked, entry)
            self.assertNotEqual(checked, path)
            self.assertTrue(checked.is_relative_to(self.root / "build/deps"))
            path.write_bytes(b"replacement after private verification")
        def build_boundary(command, cwd, log):
            self.assertEqual((cwd / "a").read_bytes(), b"test")
            raise ValueError("synthetic build boundary; no execution")
        with patch.object(helper, "native_target", return_value="linux-x86_64"), \
                patch.object(helper.shutil, "which", return_value="fixture-tool"), \
                patch.object(helper.subprocess, "run", return_value=SimpleNamespace(returncode=0)), \
                patch.object(helper, "verify_archive", side_effect=replace_original), \
                patch.object(helper, "run", side_effect=build_boundary) as execute:
            with self.assertRaisesRegex(ValueError, "synthetic build boundary"):
                helper.bootstrap(self.root, path, False, self.root / "build/crypto", 1)
        execute.assert_called_once()
        self.assertFalse((self.root / "build/crypto").exists())

    def test_rejects_unsafe_entries_before_writing_anything(self):
        for name, kind in [
            ("openssl-test/../../escape", tarfile.REGTYPE),
            ("/openssl-test/escape", tarfile.REGTYPE),
            ("other/a", tarfile.REGTYPE),
            ("openssl-test/a", tarfile.SYMTYPE),
            ("openssl-test/a", tarfile.LNKTYPE),
            ("openssl-test/a", tarfile.FIFOTYPE),
            ("openssl-test/a", tarfile.CHRTYPE),
            ("openssl-test/back\\slash", tarfile.REGTYPE),
            ("openssl-test/new\nline", tarfile.REGTYPE),
            ("openssl-test/good", tarfile.REGTYPE),
        ]:
            with self.subTest(name=name, kind=kind):
                path = self.archive([("openssl-test/good", tarfile.REGTYPE), (name, kind)])
                out = self.root / "out"
                out.mkdir(exist_ok=True)
                with self.assertRaises(ValueError):
                    helper.extract_verified(path, out, "openssl-test")
                self.assertEqual(list(out.iterdir()), [])

    def test_outputs_stay_inside_build_and_do_not_follow_symlinks(self):
        (self.root / "build").mkdir()
        allowed = self.root / "build/deps/local"
        self.assertEqual(helper.checked_output_path(self.root, allowed), allowed)
        for rejected in (self.root / "outside", self.root / "build", self.root / "build/../outside"):
            with self.assertRaises(ValueError):
                helper.checked_output_path(self.root, rejected)
        (self.root / "build/link").symlink_to(self.root)
        with self.assertRaises(ValueError):
            helper.checked_output_path(self.root, self.root / "build/link/deps")

    def test_existing_prefix_stops_before_download_or_build(self):
        prefix = self.root / "build/crypto"
        prefix.mkdir(parents=True)
        (prefix / "keep").write_text("existing")
        with patch.object(helper, "native_target", return_value="linux-x86_64"), \
                patch.object(helper, "download") as network, patch.object(helper, "run") as execute:
            with self.assertRaisesRegex(ValueError, "already exists"):
                helper.bootstrap(self.root, None, True, prefix, 1)
        network.assert_not_called()
        execute.assert_not_called()
        self.assertEqual((prefix / "keep").read_text(), "existing")

    def test_missing_perl_module_fails_before_download_or_build(self):
        with patch.object(helper, "native_target", return_value="linux-x86_64"), \
                patch.object(helper.shutil, "which", return_value="fixture-tool"), \
                patch.object(helper.subprocess, "run", return_value=SimpleNamespace(returncode=1)), \
                patch.object(helper, "download") as network, patch.object(helper, "run") as execute:
            with self.assertRaisesRegex(ValueError, "Time::Piece"):
                helper.bootstrap(self.root, None, True, self.root / "build/crypto", 1)
        network.assert_not_called()
        execute.assert_not_called()
        self.assertFalse((self.root / "build").exists())

    def test_only_https_official_release_destinations(self):
        for url in ("https://github.com/openssl/openssl/releases/a", "https://release-assets.githubusercontent.com/a"):
            self.assertEqual(helper.checked_https(url), url)
        for url in ("http://github.com/a", "file:///tmp/a", "https://github.com.evil.test/a",
                    "https://github.com@evil.test/a", "https://user@github.com/a", "https://github.com:444/a"):
            with self.assertRaises(ValueError):
                helper.checked_https(url)

    def test_platform_targets_are_explicit(self):
        for platform_name, machine, expected in [
            ("Linux", "x86_64", "linux-x86_64"), ("Linux", "aarch64", "linux-aarch64"),
            ("Darwin", "arm64", "darwin64-arm64-cc"), ("Darwin", "x86_64", "darwin64-x86_64-cc")]:
            with patch.object(helper.platform, "system", return_value=platform_name), \
                    patch.object(helper.platform, "machine", return_value=machine):
                self.assertEqual(helper.native_target(), expected)
        with patch.object(helper.platform, "system", return_value="Windows"):
            with self.assertRaises(ValueError):
                helper.native_target()

    def test_relocatable_paths_preserve_hardening_without_staging_path(self):
        manifest = json.loads((Path(__file__).resolve().parents[1] /
                               "third_party/openssl-source.json").read_text())
        stage = self.root / "build/packaging crypto"
        before = list(manifest["build_configuration"]["configure_arguments"])
        args = helper.configure_arguments(manifest, "darwin64-arm64-cc", stage, True)
        self.assertIn("--prefix=/ovmesh/disabled", args)
        self.assertIn("--openssldir=/ovmesh/disabled/ssl", args)
        self.assertFalse(any(str(self.root) in arg or "${" in arg for arg in args))
        self.assertEqual(args[0], "darwin64-arm64-cc")
        self.assertEqual([arg for arg in args if arg.startswith("no-")],
                         [arg for arg in before if arg.startswith("no-")])
        self.assertEqual(manifest["build_configuration"]["configure_arguments"], before)
        self.assertEqual(helper.checked_output_path(self.root, stage), stage)
        self.assertFalse(stage.exists())

    def test_default_compiled_paths_remain_local_and_unknown_variables_fail(self):
        manifest = {"build_configuration": {"configure_arguments": [
            "${OPENSSL_TARGET}", "--prefix=${REPO_ROOT}/build/crypto",
            "--openssldir=${REPO_ROOT}/build/crypto/ssl"]}}
        stage = self.root / "build/crypto"
        args = helper.configure_arguments(manifest, "linux-x86_64", stage)
        self.assertEqual(args, ["linux-x86_64", "--prefix=" + str(stage),
                               "--openssldir=" + str(stage / "ssl")])
        manifest["build_configuration"]["configure_arguments"].append("${UNRESOLVED}")
        for enabled in (False, True):
            with self.assertRaisesRegex(ValueError, "unresolved"):
                helper.configure_arguments(manifest, "linux-x86_64", stage, enabled)


if __name__ == "__main__":
    unittest.main()
