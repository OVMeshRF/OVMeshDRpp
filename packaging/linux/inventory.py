#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Public package inventory; deliberately excludes local build receipts/paths."""
import json
import hashlib
from pathlib import Path
import re
import subprocess
import sys

def output(*args):
    return subprocess.check_output(args, text=True).strip()

identity = json.loads(Path("/src/source-identity.json").read_text())
assert set(identity) == {"commit", "tree"}
assert all(re.fullmatch(r"[0-9a-f]{40}", value) for value in identity.values())
packaging_files = ("Dockerfile", "README.md", "build-local.py", "build.sh", "inspect-stage.py", "inventory.py",
                   "org.ovmesh.OVMeshDRpp.desktop", "package.sh", "test-build.sh", "test-package.sh")
packaging_hashes = {name: hashlib.sha256((Path("/packaging") / name).read_bytes()).hexdigest()
                    for name in packaging_files}
inventory = {
    "product": "OVMeshDRpp", "version": "0.4.1", "package_revision": 1,
    "exported_source_commit": identity["commit"],
    "exported_source_tree": identity["tree"],
    "packaging_material_sha256": packaging_hashes,
    "baseline": "Ubuntu 24.04", "architecture": output("dpkg", "--print-architecture"),
    "libc": output("getconf", "GNU_LIBC_VERSION"),
    "compiler": output("c++", "-dumpfullversion"),
    "cmake": output("cmake", "--version").splitlines()[0],
    "components": {"OpenSSL libcrypto": "3.5.8 (static)", "libusb": "1.0.30 (shared, replaceable)",
        "libhackrf": "2024.02.1 (static)", "librtlsdr": "2.0.3 (reviewed static subset)",
        "SQLite": "3.53.4 (static)", "Nanopb": "0.4.9.2 (decode-only, nesting limit 8)",
        "Semtech HAL": "2.1.0 (separate experimental RAK worker)",
        "Dear ImGui": "1.92.9b (static)", "GLFW": "3.5.1 (static)",
        "Meshtastic schema": "2.8.0 (generated decode-only descriptors)",
        "SDRangel DSP": "866ef1656af7e0923554581afc5c6dd97cbafbaf (selected adaptations)"},
    "source_inventory": "source-manifests/ and matching companion source archive",
    "runtime_packages_at_build": output("dpkg-query", "-W", "libc6", "libstdc++6", "libgcc-s1",
        "libgl1", "libopengl0", "libx11-6", "libxrandr2", "libxinerama1", "libxcursor1", "libxi6", "xdg-utils").splitlines(),
    "limits": ["No USB, GPS or RF acceptance performed", "No native Linux desktop acceptance",
        "No guarantee of real-time full-span operation", "Desktop LoRa decoding is disabled"],
}
Path(sys.argv[1]).write_text(json.dumps(inventory, indent=2) + "\n")
