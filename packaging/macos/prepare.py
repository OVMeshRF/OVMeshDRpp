#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage an arm64 macOS review bundle; never Developer-ID sign or submit."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
APP_NAME = 'OVMeshDRpp.app'


def run(*args):
    return subprocess.check_output(args, cwd=ROOT, text=True, stderr=subprocess.STDOUT)


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inputs', type=Path, default=ROOT / 'build/inputs')
    parser.add_argument('--usb-prefix', type=Path, default=ROOT / 'build/deps/usb-1.0.30-local')
    args = parser.parse_args()
    inputs = args.inputs.resolve(strict=True)
    if sys.platform != 'darwin' or os.uname().machine != 'arm64':
        raise ValueError('This packaging recipe requires native macOS arm64.')
    git_root = Path(run('git', 'rev-parse', '--show-toplevel').strip()).resolve()
    if git_root != ROOT.resolve():
        raise ValueError('Source must be the root of its own isolated Git repository; enclosing repositories are refused.')
    if run('git', 'status', '--porcelain').strip():
        raise ValueError('Commit reviewed changes before creating matching source.')
    output = ROOT / 'build/macos-package'
    if output.exists():
        raise ValueError('Output already exists; preserve it and use a fresh checkout/build.')
    native = ROOT / 'build/native-release'
    usb = args.usb_prefix.resolve(strict=True) / 'lib/libusb-1.0.0.dylib'
    entries = [json.loads((ROOT / 'third_party/openssl-source.json').read_text())['files'][0]]
    entries += json.loads((ROOT / 'third_party/usb-source.json').read_text())['components']
    for entry in entries:
        source = inputs / entry['name']
        if source.stat().st_size != entry['bytes'] or digest(source) != entry['sha256']:
            raise ValueError('Source archive mismatch: ' + entry['name'])
    output.mkdir(mode=0o700, parents=True)
    image = output / 'image'
    image.mkdir()
    app = image / APP_NAME
    # CMake install supplies the exact same complete license texts embedded by the app.
    run('cmake', '--install', str(native), '--prefix', str(output / 'install'))
    shutil.copytree(output / 'install' / APP_NAME, app)
    frameworks = app / 'Contents/Frameworks'
    frameworks.mkdir()
    library = frameworks / usb.name
    shutil.copyfile(usb, library)
    library.chmod(0o755)
    resources = app / 'Contents/Resources'
    resources.mkdir(exist_ok=True)
    shutil.copytree(output / 'install/share/OVMeshDRpp', resources / 'Licenses')
    plist_path = app / 'Contents/Info.plist'
    plist = plistlib.loads(plist_path.read_bytes())
    plist.update(CFBundleVersion='0.4.1', LSMinimumSystemVersion='13.0',
                 NSHighResolutionCapable=True,
                 NSHumanReadableCopyright='Copyright 2026 OVMeshDRpp contributors; GPL-3.0-or-later')
    plist_path.write_bytes(plistlib.dumps(plist, sort_keys=True))
    executable = app / 'Contents/MacOS/OVMeshDRpp'
    worker = app / 'Contents/MacOS/ovmesh-rak-worker'
    if not worker.is_file():
        raise ValueError('Expected RAK worker is missing.')
    run('install_name_tool', '-change', str(usb),
        '@executable_path/../Frameworks/' + usb.name, str(executable))
    run('install_name_tool', '-id', '@rpath/' + usb.name, str(library))
    binaries = [library, worker, executable]
    inventory = []
    for binary in binaries:
        # Strip local debug symbols and reapply ordinary local ad-hoc signatures.
        run('strip', '-S', str(binary))
        loads = run('otool', '-L', str(binary))
        commands = run('otool', '-l', str(binary))
        arch = run('lipo', '-archs', str(binary)).strip()
        minimums = re.findall(r'^\s*minos (\S+)', commands, re.M)
        if arch != 'arm64' or minimums != ['13.0']:
            raise ValueError('Unexpected architecture/minimum version: ' + str(binary))
        for line in loads.splitlines()[1:]:
            name = line.strip().split(' (', 1)[0]
            if not name.startswith(('/usr/lib/', '/System/Library/', '@rpath/libusb-1.0.0.dylib',
                                    '@executable_path/../Frameworks/libusb-1.0.0.dylib')):
                raise ValueError('Unexpected dynamic dependency: ' + name)
        if 'LC_RPATH' in commands:
            raise ValueError('Unexpected RPATH requires explicit review.')
        strings = run('strings', str(binary))
        if any(path in strings for path in (str(ROOT), '/Users/', '/opt/homebrew/', '/private/tmp/')):
            raise ValueError('Build/private path remains in binary: ' + binary.name)
        run('codesign', '--force', '--sign', '-', str(binary))
        inventory.append({'path': str(binary.relative_to(image)), 'architecture': arch,
                          'minimum_macos': minimums[0], 'load_commands': loads.splitlines()[1:]})
    run('codesign', '--force', '--sign', '-', str(app))
    run('codesign', '--verify', '--deep', '--strict', str(app))
    sources = image / 'Sources'
    sources.mkdir()
    commit = run('git', 'rev-parse', 'HEAD').strip()
    run('git', 'archive', '--format=tar.gz', '--prefix=OVMeshDRpp-0.4.1/',
        '--output=' + str(sources / 'OVMeshDRpp-0.4.1-source.tar.gz'), commit)
    for entry in entries:
        shutil.copyfile(inputs / entry['name'], sources / entry['name'])
    shutil.copyfile(ROOT / 'packaging/macos/README.md', sources / 'MACOS-BUILD.md')
    (image / 'Applications').symlink_to('/Applications')
    (image / 'Read Me.txt').write_text(
        'OVMeshDRpp 0.4.1 — Apple Silicon / macOS 13.0 or later\n\n'
        'Drag OVMeshDRpp.app to Applications when using an approved signed release.\n'
        'This review candidate is ad-hoc signed, not Developer-ID signed or notarized.\n'
        'Tested on macOS 26.3 only; older macOS and packaged hardware remain unqualified.\n'
        'Experimental local receive-only spectrum surveys. Desktop LoRa decoding is disabled.\n'
        'No semantic message content is retained. Field characterization remains incomplete.\n\n'
        'Ordinary launch can connect an enabled recognized GPS. Packaging validation uses\n'
        'explicit --ui-smoke / --demo modes. No automatic RF reception occurs at startup.\n'
        'Complete licenses are inside the app Resources and About & licenses.\n'
        'Sources contains matching application/build source and exact dependency archives.\n'
        'See Sources/MACOS-BUILD.md for reproduction and shared libusb replacement.\n')
    # Record hashes after the final ad-hoc seal; never include private build logs.
    for item in inventory:
        item['sha256'] = digest(image / item['path'])
    manifest = {'version': '0.4.1', 'source_commit': commit,
                'signing': 'ad-hoc review only', 'binaries': inventory,
                'sources': {p.name: digest(p) for p in sorted(sources.iterdir())}}
    (image / 'PACKAGE-MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n')
    # Normalize staged permissions; no private working-directory paths enter the manifest.
    for path in image.rglob('*'):
        if not path.is_symlink():
            path.chmod(0o755 if path.is_dir() or path in binaries else 0o644)
    run('codesign', '--verify', '--deep', '--strict', str(app))
    print(output)


if __name__ == '__main__':
    main()
