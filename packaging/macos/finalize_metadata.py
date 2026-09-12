#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Refresh disk-image metadata after separately authorized signing/checks.

Never signs, submits, installs, changes app contents, or creates a disk image.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--team-id', required=True)
    parser.add_argument('--notarized-app', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    image = args.image.resolve(strict=True)
    if not image.is_relative_to(root / 'build'):
        raise ValueError('Image staging must be inside this checkout build directory.')
    if not re.fullmatch(r'[A-Z0-9]{10}', args.team_id):
        raise ValueError('Expected the reviewed ten-character signing Team ID.')
    app = image / 'OVMeshDRpp.app'
    manifest_path = image / 'PACKAGE-MANIFEST.json'
    manifest = json.loads(manifest_path.read_text())
    expected = {'OVMeshDRpp.app/Contents/MacOS/OVMeshDRpp',
                'OVMeshDRpp.app/Contents/MacOS/ovmesh-rak-worker',
                'OVMeshDRpp.app/Contents/Frameworks/libusb-1.0.0.dylib'}
    if {item['path'] for item in manifest['binaries']} != expected:
        raise ValueError('Unexpected manifest binary inventory.')
    run('codesign', '--verify', '--deep', '--strict', str(app))
    for item in manifest['binaries']:
        binary = image / item['path']
        run('codesign', '--verify', '--strict', str(binary))
        signature = run('codesign', '--display', '--verbose=4', str(binary))
        if ('Authority=Developer ID Application:' not in signature or
                'TeamIdentifier=' + args.team_id not in signature or
                '(runtime)' not in signature or 'Timestamp=' not in signature):
            raise ValueError('Expected timestamped Developer ID hardened-runtime signature.')
    status = 'Developer ID signed; app notarization not yet verified'
    if args.notarized_app:
        run('xcrun', 'stapler', 'validate', str(app))
        run('spctl', '--assess', '--type', 'execute', '--verbose=4', str(app))
        status = 'Developer ID signed; app notarization ticket stapled and verified'
    # Check all prerequisites before writing status or hashes. The app is unchanged.
    for item in manifest['binaries']:
        item['sha256'] = digest(image / item['path'])
    manifest['signing'] = status
    manifest['team_id'] = args.team_id
    manifest['sources'] = {p.name: digest(p) for p in sorted((image / 'Sources').iterdir()) if p.is_file()}
    readme = image / 'Read Me.txt'
    lines = readme.read_text().splitlines()
    lines = [line for line in lines if not line.startswith((
        'This review candidate is ', 'Signature status: ', 'Disk-image trust: '))]
    lines[2:2] = ['Signature status: ' + status + '.',
                  'Disk-image trust: final DMG verification is recorded separately.', '']
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    readme.write_text('\n'.join(lines) + '\n')
    print(status + '; refreshed staged hashes; no app or DMG signed/submitted')


if __name__ == '__main__':
    main()
