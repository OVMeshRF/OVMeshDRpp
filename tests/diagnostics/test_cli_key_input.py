#!/usr/bin/env python3
"""Local CLI key-intake regressions. Every receive case forces invalid rate 0.

The engine must reject that rate before any HackRF library initialization.
This script uses Python standard library only; it is not an application dependency.
Usage: python3 tests/diagnostics/test_cli_key_input.py build/native/ovmesh-cli
"""
import os
from pathlib import Path
import subprocess
import sys

binary = str(Path(sys.argv[1]).resolve())
base = [binary, "--receive-hackrf", "--confirm-radio-access", "--sample-rate", "0"]
checked = 0

def check(args, key, expected):
    global checked
    result = subprocess.run(args, input=key, capture_output=True, timeout=5)
    assert result.returncode == 1 and expected in result.stderr, (args, result.returncode, result.stderr)
    if key.strip():
        assert key.strip() not in result.stdout + result.stderr, "Input leaked in diagnostics"
    checked += 1

for valid in (b"AQ==\n", b"AQ==", b"000102030405060708090a0b0c0d0e0f\n",
              b"AAECAwQFBgcICQoLDA0ODw==\n", b"00" * 32 + b"\n"):
    check(base + ["--channel-key-stdin", "1,LongFast"], valid, b"sample rate")
for invalid in (b"", b"\n", b"AR==\n", b"AQ== \n", b"AQ==\r\n", b"AQ\x00==\n", b"AQ==\x1a", b"A" * 65 + b"\n"):
    check(base + ["--channel-key-stdin", "1,LongFast"], invalid, b"Invalid redirected")
for option, expected in (("0,LongFast", b"1 through 4"), ("5,LongFast", b"1 through 4"),
                         ("2,LongFast", b"must exist"), ("1,", b"nonempty"),
                         ("1," + "x" * 81, b"at most 80"), ("1,a,b", b"requires LANE")):
    check(base + ["--channel-key-stdin", option], b"AQ==\n", expected)
check([binary, "--headless-demo", "--channel-key-stdin", "1,LongFast"], b"AQ==\n", b"explicit HackRF")
check([binary, "--receive-hackrf", "--channel-key-stdin", "1,LongFast"], b"AQ==\n", b"requires explicit")
check(base + ["--channel-key-stdin", "1,LongFast", "--channel-key-stdin", "1,LongFast"], b"AQ==\n", b"Only one")
for slot in (1, 8, 16):
    check(base + ["--survey-key-stdin", f"{slot},Authorized survey key"], b"AQ==\n", b"sample rate")
for option, expected in (("0,Key", b"1 through 16"), ("17,Key", b"1 through 16"),
                         ("1,", b"nonempty"), ("1," + "x" * 81, b"at most 80"),
                         ("1,label,extra", b"requires SLOT")):
    check(base + ["--survey-key-stdin", option], b"AQ==\n", expected)
check(base + ["--survey-key-stdin", "1,Survey", "--channel-key-stdin", "1,LongFast"], b"AQ==\n", b"Only one")
check(base + ["--survey-key-stdin", "1,Survey"], b"AR==\n", b"Invalid redirected")
check([binary, "--headless-demo", "--survey-key-stdin", "1,Survey"], b"AQ==\n", b"explicit HackRF")
if os.name == "posix":
    import pty
    master, slave = pty.openpty()
    try:
        result = subprocess.run(base + ["--channel-key-stdin", "1,LongFast"], stdin=slave,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        assert result.returncode == 1 and b"interactive entry is disabled" in result.stderr
        checked += 1
    finally:
        os.close(master)
        os.close(slave)
print(f"PASS: {checked} bounded CLI key-intake cases; invalid rate prevents radio access")
