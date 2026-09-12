#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inspect the explicit Linux package payload and its ELF install paths."""
from pathlib import Path
import re
import subprocess
import sys

stage = Path(sys.argv[1])
required = {"usr/bin/OVMeshDRpp", "usr/bin/ovmesh-cli", "usr/bin/ovmesh-rak-worker",
    "usr/lib/ovmeshdrpp/libusb-1.0.so.0",
    "usr/share/applications/org.ovmesh.OVMeshDRpp.desktop",
    "usr/share/OVMeshDRpp/LICENSES.txt", "usr/share/OVMeshDRpp/third_party/LIBUSB_COPYING.txt",
    "usr/share/doc/ovmeshdrpp/build-inventory.json"}
for name in required:
    assert (stage / name).is_file(), name
elf_count = 0
for path in stage.rglob("*"):
    name = path.relative_to(stage).as_posix()
    assert not any(part in {".git", ".codex", ".agents", "build", "__pycache__"}
                   for part in path.relative_to(stage).parts), name
    assert path.suffix not in {".sqlite", ".sqlite3", ".db", ".iq", ".log", ".pem", ".key"}, name
    if path.is_symlink():
        assert path.parent == stage / "usr/lib/ovmeshdrpp", name
        assert path.resolve().parent == path.parent.resolve(), name
        continue
    if not path.is_file():
        continue
    data = path.read_bytes()
    assert b"/Users/" not in data, name
    if not data.startswith(b"\x7fELF"):
        continue
    elf_count += 1
    assert b"/src/build/deps/" not in data, name
    dynamic = subprocess.check_output(["readelf", "-d", str(path)], text=True)
    assert "(RPATH)" not in dynamic, name
    assert "/src/" not in dynamic, name
    assert not re.search(r"NEEDED.*lib(?:crypto|ssl|hackrf|rtlsdr|sqlite3)", dynamic), name
    if path.parent == stage / "usr/bin":
        assert "[$ORIGIN/../lib/ovmeshdrpp]" in dynamic, name
    assert not path.stat().st_mode & 0o6000, name
assert elf_count == 4, elf_count
print("PASS: explicit package payload, 4 ELF files, relative RUNPATH, shared libusb and no host/build-prefix paths")
