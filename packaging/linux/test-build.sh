#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
# Both paths must be backed by native Linux tmpfs, not a macOS bind mount.
test "$(stat -f -c %T build/fixtures)" = tmpfs
test "$(stat -f -c %T build/publication-review)" = tmpfs
test_dir=$(mktemp -d "$PWD/build/fixtures/ctest.XXXXXX")
# Preserve generated test commands and assertions; change only fixture location.
python3 - "$test_dir" <<'PY'
from pathlib import Path
import shutil
import sys
native = Path('build/native')
destination = Path(sys.argv[1])
for path in native.rglob('CTestTestfile.cmake'):
    target = destination / path.relative_to(native)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(path, target)
PY
case "${1:-full}" in
    full) ctest --test-dir "$test_dir" --output-on-failure -j 2 \
        --output-junit "$PWD/build/evidence/ctest-full.xml" ;;
    crypto-relink) ctest --test-dir "$test_dir" --output-on-failure -j 2 \
        -R '^(crypto_intake|protocol|engine|engine_detailed|storage|compact_storage|reports|discovery_storage)$' \
        --output-junit "$PWD/build/evidence/ctest-crypto-relink.xml" ;;
    *) echo 'Choose full or crypto-relink' >&2; exit 1 ;;
esac
