# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline publication guard tests; all example values are deliberately fake."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/audit_publication.py"
spec = importlib.util.spec_from_file_location("publication_audit", SCRIPT)
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class PublicationAuditTests(unittest.TestCase):
    def test_findings_do_not_echo_fake_secret(self):
        fake = "ghp_" + "x" * 36
        hits, _ = audit.inspect_blob(fake.encode())
        self.assertEqual(hits[0]["category"], "provider_token")
        self.assertNotIn(fake, json.dumps(hits))

    def test_private_key_and_personal_path(self):
        text = ("-----BEGIN " + "PRIVATE KEY-----\n" + "/Users/" + "fictional-person/project/").encode()
        hits, _ = audit.inspect_blob(text)
        self.assertEqual({h["category"] for h in hits}, {"private_key", "personal_home_path"})

    def test_operational_filenames(self):
        for name in ("capture.sqlite", "track.gpx", "output.geojson", "preferences.conf", ".env.local"):
            self.assertTrue(audit.SENSITIVE_PATH.search(name))
        self.assertFalse(audit.SENSITIVE_PATH.search("tests/test_gps.cpp"))

    def test_public_examples_still_need_manual_review(self):
        hits, reviews = audit.inspect_blob(("$" + "GPRMC," + "0," * 12).encode())
        self.assertFalse(hits)
        self.assertTrue(reviews)
        _, reviews = audit.inspect_blob(b"\x89PNG\0")
        self.assertTrue(reviews)

    def test_old_deleted_value_remains_visible_in_history(self):
        base = ROOT / "build/publication-review"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as directory:
            def git(*args):
                return subprocess.run(["git", *args], cwd=directory, check=True, capture_output=True)
            git("init", "--quiet")
            git("config", "user.name", "Synthetic fixture")
            git("config", "user.email", "fixture@users.noreply.github.com")
            path = Path(directory) / "fixture.txt"
            fake = "ghp_" + "z" * 36
            path.write_text(fake)
            git("add", "fixture.txt")
            git("-c", "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "Synthetic fixture")
            path.write_text("No credentials here.\n")
            git("add", "fixture.txt")
            clean = subprocess.run([sys.executable, "-B", str(SCRIPT)], cwd=directory, capture_output=True, text=True)
            self.assertEqual(clean.returncode, 0, clean.stderr)
            history = subprocess.run([sys.executable, "-B", str(SCRIPT), "--all-objects"], cwd=directory, capture_output=True, text=True)
            self.assertEqual(history.returncode, 1, history.stderr)
            self.assertEqual(json.loads(history.stdout)["category_counts"]["provider_token"], 1)
            self.assertNotIn(fake, history.stdout)


if __name__ == "__main__":
    unittest.main()
