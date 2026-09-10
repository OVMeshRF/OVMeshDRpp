#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Export an explicitly reviewed Git tree without history or unlisted files.

Offline and standard-library only. This is a packaging boundary, not proof that
listed contents are safe; source, fixture, metadata and image review is required.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import unicodedata


MANIFEST = "tools/public-source.json"
BLOCKED_PARTS = {".git", ".codex", ".agents", "build", "dist", "out",
                 "private-surveys", "private-iq", "captures", "exports", "secrets"}
BLOCKED_NAMES = {"agents.md", "preferences.conf", ".env"}
BLOCKED_SUFFIXES = {".sqlite", ".sqlite3", ".db", ".log", ".iq", ".iq8",
                    ".nmea", ".gpx", ".kml", ".kmz", ".ubx", ".pcap",
                    ".pcapng", ".csv", ".geojson", ".pem", ".key", ".p12", ".pfx"}
WINDOWS_RESERVED = {"con", "prn", "aux", "nul", "clock$", "conin$", "conout$"} | {
    prefix + number for prefix in ("com", "lpt") for number in "123456789\u00b9\u00b2\u00b3"}
WINDOWS_ILLEGAL = set('<>:"\\|?*')


def git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args])


def unique_json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate manifest object member")
        result[key] = value
    return result


def manifest_paths(data):
    manifest = json.loads(data, object_pairs_hook=unique_json_object)
    if (not isinstance(manifest, dict) or not {"version", "files"}.issubset(manifest) or
            set(manifest) - {"version", "files", "description"} or
            type(manifest["version"]) is not int or manifest["version"] != 1 or
            ("description" in manifest and not isinstance(manifest["description"], str))):
        raise ValueError("Unsupported public-source manifest")
    files = manifest["files"]
    if not isinstance(files, list) or not files or any(not isinstance(p, str) for p in files):
        raise ValueError("Manifest needs explicit file paths")
    if MANIFEST not in files:
        raise ValueError("Missing public-source manifest")
    folded_files = set()
    prefixes = {}
    for name in files:
        path = PurePosixPath(name)
        if (not name or path.is_absolute() or str(path) != name or not path.parts or
                unicodedata.normalize("NFC", name) != name or
                any(ord(c) < 32 or ord(c) == 127 or 0xd800 <= ord(c) <= 0xdfff for c in name)):
            raise ValueError("Disallowed public-source path")
        for part in path.parts:
            folded = part.casefold()
            # Windows accepts several spellings for the same output location.
            # Reject aliases instead of silently stripping or normalizing them.
            if (part in (".", "..") or part.endswith((".", " ")) or
                    any(c in WINDOWS_ILLEGAL for c in part) or folded in BLOCKED_PARTS or
                    folded in BLOCKED_NAMES or folded.startswith(".env.") or
                    folded.split(".", 1)[0].rstrip(" ") in WINDOWS_RESERVED or
                    PurePosixPath(part).suffix.casefold() in BLOCKED_SUFFIXES):
                raise ValueError("Disallowed public-source path")
        key = name.casefold()
        if key in folded_files:
            raise ValueError("Duplicate or case-alias public-source path")
        folded_files.add(key)
        # Reject case aliases in directory components too (Docs/a and docs/b).
        for length in range(1, len(path.parts) + 1):
            prefix = "/".join(path.parts[:length])
            previous = prefixes.setdefault(prefix.casefold(), prefix)
            if previous != prefix:
                raise ValueError("Case-alias public-source directory")
    for name in files:
        if any(str(parent).casefold() in folded_files for parent in PurePosixPath(name).parents
               if str(parent) != "."):
            raise ValueError("File and directory collide in public-source manifest")
    return files


def prepare(repo, ref, destination):
    repo = Path(repo).resolve()
    destination = Path(destination)
    # Ref is resolved before constructing any object expression or path.
    commit = git(repo, "rev-parse", "--verify", "--end-of-options",
                 ref + "^{commit}").decode().strip()
    files = manifest_paths(git(repo, "show", commit + ":" + MANIFEST))
    entries = {}
    for row in git(repo, "ls-tree", "-r", "-z", commit).split(b"\0"):
        if row:
            meta, name = row.split(b"\t", 1)
            mode, kind, oid = meta.decode().split()
            entries[name.decode()] = (mode, kind, oid)
    selected = []
    for name in files:
        entry = entries.get(name)
        if entry is None or entry[0] not in ("100644", "100755") or entry[1] != "blob":
            raise ValueError("Missing path, symlink, or submodule in public manifest")
        data = git(repo, "cat-file", "blob", entry[2])
        selected.append((name, entry[0], data))
    # Reject existing output rather than merging with stale/private files.
    # Reject symlink parents so an output path cannot redirect into another tree.
    for parent in (destination, *destination.parents):
        if parent.is_symlink():
            raise ValueError("Symlink output path is not permitted")
    destination.mkdir(mode=0o700, parents=False, exist_ok=False)
    digest = hashlib.sha256()
    for name, mode, data in selected:
        target = destination / name
        target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        with target.open("xb") as stream:
            stream.write(data)
        target.chmod(0o755 if mode == "100755" else 0o644)
        digest.update(name.encode() + b"\0" + mode.encode() + b"\0")
        digest.update(hashlib.sha256(data).digest())
    return {"files": len(selected), "source_commit": commit,
            "content_manifest_sha256": digest.hexdigest(), "history_included": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--ref", default="HEAD")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = prepare(args.repo, args.ref, args.output)
    except (OSError, ValueError, subprocess.CalledProcessError):
        parser.exit(1, "Public-source export failed; no publication clearance produced.\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
