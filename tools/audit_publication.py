#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline publication triage. Prints locations/categories, never matched values.

Default: exact staged Git blobs. --all-objects: every local blob, including old
and unreachable versions, plus a commit-email privacy count. No network, device
access, live-data reads, Git mutation or automatic deletion. Human review of
fixtures, images, coordinates and publication identity remains required.
"""
import argparse
import collections
import json
import re
import subprocess
import sys


RULES = {
    "private_key": rb"-----BEGIN (?:RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----",
    "provider_token": rb"\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|AKIA[A-Z0-9]{16}|xox[baprs]-[A-Za-z0-9-]{16,}|AIza[A-Za-z0-9_-]{30,}|sk-(?:proj-)?[A-Za-z0-9_-]{32,})\b",
    "url_credentials": rb"[a-zA-Z]+://[^\s/:]+:[^\s/@]+@",
    "personal_home_path": rb"/(?:Users|home)/(?!Shared/|USER/|<|\$)[^/\s\x22\x27]+/",
    "serial_derived_identifier": rb"usbmodem[A-Fa-f0-9]{10,}",
    "credential_assignment": rb"(?i)(?:api[_-]?key|access[_-]?token|password|passwd|secret|private[_-]?key|channel[_-]?key)\s*[=:]\s*[\x22\x27][^\x22\x27\r\n]{8,}[\x22\x27]",
}
NMEA = re.compile(rb"\$G[A-Z](?:GGA|RMC),[^\r\n]{15,}")
# Only generic path examples in these exact reviewed SQLite blobs are classified
# as public documentation. All other rules still run, and any edit changes the
# object ID and removes this exception. Never exempt an entire vendor directory.
PUBLIC_SQLITE_PATH_EXAMPLES = {
    "0644a39f8734fdef1506609af760147454e15389",
    "c91b81c08f55c362db136d3b0c123b8c03021423",
}
SENSITIVE_PATH = re.compile(
    r"(?i)(?:\.(?:sqlite3?|db|iq8?|cfile|sigmf-data|sigmf-meta|pcapng?|nmea|nme|ubx|gpx|kml|kmz|csv|geojson|log|pem|key|p12|pfx)$|"
    r"(?:^|/)\.env(?:\.|$)|(?:^|/)(?:preferences\.conf|private-surveys|private-iq|captures|exports|secrets)(?:/|$))")


def git(*args):
    return subprocess.check_output(["git", *args])


def inspect_blob(data):
    findings = []
    for category, pattern in RULES.items():
        for match in re.finditer(pattern, data):
            findings.append({"category": category, "line": data[:match.start()].count(b"\n") + 1})
    reviews = []
    if NMEA.search(data):
        reviews.append("NMEA literal: verify public/synthetic fixture provenance")
    if b"\0" in data:
        reviews.append("binary: inspect content and embedded metadata separately")
    return findings, reviews


def scan(all_objects=False):
    entries = []
    if all_objects:
        names = {}
        for line in git("rev-list", "--objects", "--all", "--reflog").decode().splitlines():
            pair = line.split(" ", 1)
            if len(pair) == 2:
                names[pair[0]] = pair[1]
        for line in git("cat-file", "--batch-all-objects", "--batch-check=%(objectname) %(objecttype)").decode().splitlines():
            oid, kind = line.split()
            if kind == "blob":
                entries.append((oid, names.get(oid, "[unreachable or unnamed blob]")))
    else:
        for entry in git("ls-files", "--stage", "-z").split(b"\0"):
            if not entry:
                continue
            header, path = entry.split(b"\t", 1)
            mode, oid, stage = header.decode().split()
            if stage != "0" or mode not in ("100644", "100755"):
                raise ValueError("Unmerged, symlink or submodule entry requires separate publication review")
            entries.append((oid, path.decode()))
    findings, reviews = [], []
    with subprocess.Popen(["git", "cat-file", "--batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE) as process:
        for oid, path in entries:
            process.stdin.write((oid + "\n").encode())
            process.stdin.flush()
            header = process.stdout.readline().split()
            if len(header) != 3 or header[1] != b"blob":
                raise ValueError("Git object read failed")
            data = process.stdout.read(int(header[2]))
            if len(data) != int(header[2]) or process.stdout.read(1) != b"\n":
                raise ValueError("Incomplete Git object read")
            hits, manual = inspect_blob(data)
            if oid in PUBLIC_SQLITE_PATH_EXAMPLES:
                examples = [item for item in hits if item["category"] == "personal_home_path"]
                if examples:
                    manual.append("reviewed public SQLite example paths in this exact pinned blob")
                hits = [item for item in hits if item["category"] != "personal_home_path"]
            for item in hits:
                findings.append({"path": path, "object": oid, **item})
            if SENSITIVE_PATH.search(path):
                findings.append({"path": path, "object": oid, "category": "operational_or_secret_filename"})
            for reason in manual:
                reviews.append({"path": path, "object": oid, "reason": reason})
        process.stdin.close()
        if process.wait() != 0:
            raise ValueError("Git reader failed")
    result = {"scope": "all_local_git_blobs" if all_objects else "staged_blobs", "blobs_scanned": len(entries),
              "findings": findings, "manual_review": reviews,
              "category_counts": dict(collections.Counter(x["category"] for x in findings))}
    if all_objects:
        emails = git("log", "--all", "--reflog", "--format=%ae%n%ce").decode().splitlines()
        private = {email for email in emails if email and not email.endswith("@users.noreply.github.com")}
        result["non_github_noreply_commit_email_count"] = len(private)
    result["limit"] = "Pattern triage only; zero matches is not proof of no secrets or private coordinates. Review fixtures, images, Git metadata and exact publication contents."
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all-objects", action="store_true")
    args = parser.parse_args()
    try:
        result = scan(args.all_objects)
        print(json.dumps(result, indent=2))
        return 1 if result["findings"] or result.get("non_github_noreply_commit_email_count") else 0
    except (ValueError, subprocess.CalledProcessError, OSError):
        print("Publication scan could not complete; no clearance was produced.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
