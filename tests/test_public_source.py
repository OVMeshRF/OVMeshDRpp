#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic publication-boundary checks; no network or operational data."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("prepare_public_source",
    Path(__file__).resolve().parents[1] / "tools/prepare_public_source.py")
exporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exporter)


class PublicSourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        # macOS may expose its temporary directory through /var -> /private/var.
        self.root = Path(self.temp.name).resolve()
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.test")
        (self.repo / "tools").mkdir()
        (self.repo / "README.md").write_text("Original public fixture\n")
        (self.repo / "AGENTS.md").write_text("Private fixture instructions\n")

    def tearDown(self):
        self.temp.cleanup()

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.repo), *args], stderr=subprocess.STDOUT)

    def commit(self, files=None):
        files = ["README.md", exporter.MANIFEST] if files is None else files
        self.commit_manifest(json.dumps({"version": 1, "files": files}))

    def commit_manifest(self, text):
        (self.repo / exporter.MANIFEST).write_text(text)
        self.git("add", "--all")
        self.git("-c", "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false",
                 "commit", "--allow-empty", "-qm", "Fixture")

    def rejected(self, reason=None):
        out = self.root / "export"
        with self.assertRaisesRegex(ValueError, reason or "."):
            exporter.prepare(self.repo, "HEAD", out)
        self.assertFalse(out.exists(), "Invalid manifest must fail before output is created")

    def test_only_committed_allowlisted_bytes_no_history(self):
        self.commit()
        (self.repo / "README.md").write_text("Unreviewed working changes\n")
        (self.repo / "scratch.txt").write_text("Untracked private fixture\n")
        out = self.root / "export"
        result = exporter.prepare(self.repo, "HEAD", out)
        self.assertEqual((out / "README.md").read_text(), "Original public fixture\n")
        self.assertEqual({str(p.relative_to(out)) for p in out.rglob("*") if p.is_file()},
                         {"README.md", exporter.MANIFEST})
        self.assertFalse((out / ".git").exists())
        self.assertFalse(result["history_included"])

    def test_private_path_cannot_be_allowlisted(self):
        self.commit(["README.md", "AGENTS.md", exporter.MANIFEST])
        self.rejected("Disallowed")

    def test_case_insensitive_private_paths_rejected(self):
        for bad in ("agents.MD", "nested/AgEnTs.Md", ".GIT/config", ".CODEX/context.md",
                    ".AGENTS/scope.md", "Build/source.cpp", "private-SURVEYS/data.txt",
                    "SECRETS/value.txt", ".ENV", ".EnV.local", "nested/Preferences.CONF",
                    "survey.SQLITE", "receiver.NMEA", "private.KEY", "capture.PCAPNG"):
            with self.subTest(path=bad):
                self.commit(["README.md", bad, exporter.MANIFEST])
                self.rejected("Disallowed")

    def test_duplicate_file_or_directory_alias_rejected(self):
        for first, second in (("README.md", "README.md"), ("README.md", "readme.MD"),
                              ("Docs/first.md", "docs/second.md"),
                              ("stra\u00dfe.md", "STRASSE.MD"),
                              ("tools/source", "tools/source/item.cpp")):
            with self.subTest(first=first, second=second):
                self.commit([first, second, exporter.MANIFEST])
                self.rejected("Duplicate|alias|collide")

    def test_windows_output_aliases_rejected(self):
        for bad in ("AGENTS.md.", "AGENTS.md ", "folder./file.cpp", "folder /file.cpp",
                    "README.md:private", "C:local.cpp", "C:/local.cpp", "a\\b.cpp",
                    "a<b.cpp", "a>b.cpp", 'a"b.cpp', "a|b.cpp", "a?b.cpp", "a*b.cpp",
                    "NUL", "con.txt", "nested/PrN.cpp", "AUX .txt", "COM1.cpp",
                    "lpt9.h", "com\u00b9.txt", "LPT\u00b2.cpp", "CoM\u00b3",
                    "CLOCK$.txt", "CONIN$", "conout$.md"):
            with self.subTest(path=bad):
                self.commit(["README.md", bad, exporter.MANIFEST])
                self.rejected("Disallowed")

    def test_malformed_or_ambiguous_paths_rejected(self):
        for bad in ("", ".", "..", "../outside.txt", "/absolute.txt", "//server/file",
                    "docs//file.md", "./README.md", "docs/./file.md", "docs/../file.md",
                    "file\nname.md", "file\x7fname.md", "file\u0301.md", "file\ud800.md"):
            with self.subTest(path=repr(bad)):
                self.commit(["README.md", bad, exporter.MANIFEST])
                self.rejected("Disallowed")

    def test_invalid_manifest_fails_closed(self):
        for manifest in ("{", "[]", '"manifest"', "null", "{}",
                         '{"version":true,"files":["tools/public-source.json"]}',
                         '{"version":1.0,"files":["tools/public-source.json"]}',
                         '{"version":2,"files":["tools/public-source.json"]}',
                         '{"version":1,"files":[]}',
                         '{"version":1,"files":"README.md"}',
                         '{"version":1,"files":[false,"tools/public-source.json"]}',
                         '{"version":1,"files":["README.md"]}',
                         '{"version":1,"files":["tools/public-source.json"],"description":false}',
                         '{"version":1,"files":["tools/public-source.json"],"unreviewed":true}',
                         '{"version":1,"version":1,"files":["tools/public-source.json"]}',
                         '{"version":1,"files":[],"files":["tools/public-source.json"]}'):
            with self.subTest(manifest=manifest):
                self.commit_manifest(manifest)
                self.rejected()

    def test_optional_manifest_description_accepted(self):
        self.commit_manifest(json.dumps({"version": 1, "description": "Public fixture list",
                                         "files": ["README.md", exporter.MANIFEST]}))
        result = exporter.prepare(self.repo, "HEAD", self.root / "export")
        self.assertEqual(result["files"], 2)

    def test_portable_unicode_paths_are_preserved(self):
        name = "docs/r\u00e9gion.md"
        (self.repo / "docs").mkdir()
        (self.repo / name).write_text("Synthetic Unicode source fixture\n")
        self.commit(["README.md", name, exporter.MANIFEST])
        out = self.root / "export"
        exporter.prepare(self.repo, "HEAD", out)
        self.assertEqual((out / name).read_text(), "Synthetic Unicode source fixture\n")

    def test_symlink_input_rejected(self):
        (self.repo / "link.md").symlink_to("AGENTS.md")
        self.commit(["README.md", "link.md", exporter.MANIFEST])
        self.rejected("symlink")

    def test_existing_output_preserved(self):
        self.commit()
        out = self.root / "export"
        out.mkdir()
        (out / "keep.txt").write_text("Keep fixture\n")
        with self.assertRaises(FileExistsError):
            exporter.prepare(self.repo, "HEAD", out)
        self.assertEqual((out / "keep.txt").read_text(), "Keep fixture\n")

    def test_symlink_output_parent_rejected(self):
        self.commit()
        real = self.root / "real"
        real.mkdir()
        link = self.root / "link"
        link.symlink_to(real, target_is_directory=True)
        with self.assertRaises(ValueError):
            exporter.prepare(self.repo, "HEAD", link / "export")
        self.assertFalse((real / "export").exists())

    def test_missing_or_traversing_path_rejected(self):
        for bad in ("missing.txt", "../outside.txt"):
            self.commit(["README.md", bad, exporter.MANIFEST])
            self.rejected()


if __name__ == "__main__":
    unittest.main()
