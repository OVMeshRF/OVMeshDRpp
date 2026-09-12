#!/usr/bin/env python3
"""Synthetic spectrum recording/readback and CLI guards; no device is opened.

Usage: python3 tests/diagnostics/test_survey_cli.py build/native/ovmesh-cli
All data is synthetic, bounded and kept in a temporary build subdirectory.
"""
from pathlib import Path
import csv
import hashlib
import re
import sqlite3
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
checked = 0


def run(args):
    return subprocess.run([binary, *args], stdin=subprocess.DEVNULL,
                          capture_output=True, text=True, timeout=15)


def reject(args, expected):
    global checked
    result = run(args)
    assert result.returncode == 1 and expected in result.stderr, (args, result.returncode, result.stderr)
    assert "Started" not in result.stdout and "Desktop control" not in result.stdout
    checked += 1


for args, expected in [
    (["--activity-threshold-dbfs", "-60"], "Receiver comparison options require"),
    (["--analyze-saved", "/not-opened.sqlite", "--activity-threshold-dbfs", "-60"], "Receiver comparison options require"),
    (["--frequency-range", "907000000,907125000"], "require --analyze-saved"),
    (["--time-range", "0,1"], "require --analyze-saved"),
    (["--bounds", "1,2,3,4"], "require --analyze-saved"),
    (["--analyze-saved", "relative.sqlite"], "absolute local path"),
    (["--analyze-saved", "/not-opened.sqlite", "--frequency-range", "1"], "wrong number"),
    (["--analyze-saved", "/not-opened.sqlite", "--frequency-range", "1,2,3"], "wrong number"),
    (["--analyze-saved", "/not-opened.sqlite", "--frequency-range", "1,"], "must not be empty"),
    (["--analyze-saved", "/not-opened.sqlite", "--frequency-range", "nan,2"], "finite"),
    (["--analyze-saved", "/not-opened.sqlite", "--frequency-range", "2,1"], "Invalid --frequency-range"),
    (["--analyze-saved", "/not-opened.sqlite", "--frequency-range", "-1,2"], "Invalid --frequency-range"),
    (["--analyze-saved", "/not-opened.sqlite", "--time-range", "2,1"], "Invalid --time-range"),
    (["--analyze-saved", "/not-opened.sqlite", "--time-range", "-1,0"], "Invalid --time-range"),
    (["--analyze-saved", "/not-opened.sqlite", "--bounds", "0,91,0,1"], "Invalid --bounds"),
    (["--analyze-saved", "/not-opened.sqlite", "--bounds", "1,0,0,1"], "Invalid --bounds"),
    (["--analyze-saved", "/not-opened.sqlite", "--headless-demo"], "Choose one"),
    (["--headless-demo", "--spectrum-only", "--lane", "907500000,125000,7,5"], "cannot be combined"),
    (["--headless-demo", "--spectrum-only", "--survey-key-stdin", "1,unused"], "cannot be combined"),
    (["--headless-demo", "--gps-device", "/not-a-device"], "--confirm-gps-access"),
    (["--confirm-gps-access"], "require --gps-device"),
    (["--gps-baud", "9600"], "require --gps-device"),
    (["--provenance"], "Export options require --export"),
    (["--report", "time"], "Export options require --export"),
    (["--export", "/not-opened.sqlite", "--report", "unknown"], "Unknown report"),
    (["--export", "/not-opened.sqlite", "--detailed-archive", "--frequency-range", "1,2"], "whole-session"),
    (["--analyze-saved", "/not-opened.sqlite", "--gps-device", "/not-a-device", "--confirm-gps-access"], "explicit reception mode"),
    (["--headless-demo", "--gps-device", "relative-device", "--confirm-gps-access"], "absolute local serial-device path"),
    (["--headless-demo", "--gps-device", "/not-a-device", "--confirm-gps-access", "--gps-baud", "1"], "Supported GPS baud rates"),
]:
    reject(args, expected)

for threshold in ("nan", "inf", "-inf", "-140.001", "0.001", "-55garbage", "1e999"):
    reject(["--headless-demo", "--spectrum-only", "--activity-threshold-dbfs", threshold],
           "requires a finite number from -140 through 0")

# Help returns without opening the named path, even with an explicit GPS option.
help_result = run(["--help", "--gps-device", "/not-a-device", "--confirm-gps-access"])
assert help_result.returncode == 0 and "--analyze-saved" in help_result.stdout
assert "--activity-threshold-dbfs" in help_result.stdout
assert "[--content]" not in help_result.stdout and "gps|content" not in help_result.stdout
reject(["--content"], "--content has been removed")
reject(["--report", "content"], "Message-content reports have been removed")


def value(output, key):
    match = re.search(r"\b" + re.escape(key) + r"=(-?[0-9.]+)", output)
    assert match, (key, output)
    return float(match.group(1))


with tempfile.TemporaryDirectory(prefix="survey-cli-", dir=Path(binary).parent) as directory:
    session = str(Path(directory) / "synthetic.sqlite")
    result = run(["--headless-demo", "--spectrum-only", "--seconds", "0.4", "--session", session,
                  "--antenna-description", "Synthetic antenna fixture", "--receiver-description", "Synthetic source",
                  "--survey-notes", "Local automated test; no RF source", "--activity-threshold-dbfs", "-62.25"])
    assert result.returncode == 0, result.stderr
    assert "decoding_scope=paused_spectrum_only" in result.stdout
    assert value(result.stdout, "activity_threshold_dbfs") == -62.25
    assert Path(session).is_file()
    with sqlite3.connect(session) as database:
        assert database.execute("PRAGMA user_version").fetchone()[0] == 6
        bin_count = database.execute("SELECT bin_count FROM spectrum_tiles LIMIT 1").fetchone()[0]
        assert database.execute("SELECT count(*) FROM window_bins").fetchone()[0] == 0
    original_digest = hashlib.sha256(Path(session).read_bytes()).digest()
    full = run(["--analyze-saved", session])
    assert full.returncode == 0 and "detailed_available=1" in full.stdout, full.stderr
    assert value(full.stdout, "observed_s") > 0
    for width in (62500, 125000, 250000, 500000, 93750):
        lower, upper = 907000000, 907000000 + width
        report = run(["--analyze-saved", session, "--frequency-range", f"{lower},{upper}"])
        assert report.returncode == 0, report.stderr
        actual_lower = value(report.stdout, "covered_lower_hz")
        actual_upper = value(report.stdout, "covered_upper_hz")
        spacing = value(report.stdout, "bin_width_hz")
        assert actual_lower <= lower and actual_upper >= upper
        assert actual_upper - actual_lower < width + 2 * spacing
        assert 0 <= value(report.stdout, "busy_s") <= value(report.stdout, "observed_s")
    empty = run(["--analyze-saved", session, "--time-range", "1000,2000"])
    assert empty.returncode == 0 and "occupancy_percent=unavailable" in empty.stdout, empty.stderr
    assert value(empty.stdout, "observed_s") == 0
    region = run(["--analyze-saved", session, "--bounds", "88,89,175,176"])
    assert region.returncode == 0 and "occupancy_percent=unavailable" in region.stdout, region.stderr
    assert "Coverage gaps have no receiver position" in region.stdout

    # Free-form provenance is an independent export privacy choice.
    ordinary_export = Path(directory) / "summary.csv"
    exported = run(["--export", session, "--output", str(ordinary_export)])
    assert exported.returncode == 0, exported.stderr
    assert "Synthetic antenna fixture" not in ordinary_export.read_text()
    with ordinary_export.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    assert len(rows) == bin_count and all(float(row['threshold_dbfs']) == -62.25 for row in rows)
    assert 'activity' not in rows[0] and 'mean_cdb' not in rows[0] and 'receiver_latitude' not in rows[0]
    assert all(int(row['quality_flags']) & 512 for row in rows)
    provenance_export = Path(directory) / "provenance.csv"
    exported = run(["--export", session, "--output", str(provenance_export), "--provenance"])
    assert exported.returncode == 0, exported.stderr
    provenance_text = provenance_export.read_text()
    assert "Synthetic antenna fixture" in provenance_text
    assert "Local automated test; no RF source" in provenance_text
    selected = Path(directory) / 'selected.csv'
    selected_args = ['--frequency-range', '907000000,907125000', '--time-range', '0.05,0.15']
    exported = run(['--export', session, '--output', str(selected), '--report', 'time',
                    '--bucket-seconds', '0.02', *selected_args])
    assert exported.returncode == 0, exported.stderr
    with selected.open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    reference = run(['--analyze-saved', session, *selected_args])
    assert reference.returncode == 0, reference.stderr
    assert abs(sum(float(row['observed_s']) for row in rows) - value(reference.stdout, 'observed_s')) < 2e-6
    assert abs(sum(float(row['busy_s']) for row in rows) - value(reference.stdout, 'busy_s')) < 2e-6
    assert all(float(row['elapsed_start_s']) >= .05 and float(row['elapsed_end_s']) <= .15 for row in rows)
    assert len(rows) <= 6
    archive = Path(directory) / 'archive.csv'
    exported = run(['--export', session, '--output', str(archive), '--detailed-archive'])
    assert exported.returncode == 0, exported.stderr
    with archive.open(newline='') as stream:
        assert any(row['record_type'] == 'session' for row in csv.DictReader(stream))
    denied = Path(directory) / 'denied.csv'
    exported = run(['--export', session, '--output', str(denied), '--report', 'gps'])
    assert exported.returncode == 1 and not denied.exists()
    assert hashlib.sha256(Path(session).read_bytes()).digest() == original_digest
    detailed = Path(directory) / 'detailed.sqlite'
    result = run(['--headless-demo', '--spectrum-only', '--seconds', '0.1',
                  '--session', str(detailed), '--detailed-recording'])
    assert result.returncode == 0, result.stderr
    with sqlite3.connect(detailed) as database:
        assert database.execute('PRAGMA user_version').fetchone()[0] == 5

    # A saved text field must not control terminal presentation. Keep the source
    # bytes intact and encode controls only at the CLI output boundary.
    reason = "source_stall\x1b[2J\r\nFORGED\x7f\u009b\u202e\\literal\u00e9"
    with sqlite3.connect(detailed) as database:
        database.execute('INSERT INTO coverage_gaps VALUES(?,?,?,?,?,?,?)',
                         (900001, 0, 1700000000.0, 1700000000.1, 0.0, 0.1, reason))
    before = hashlib.sha256(detailed.read_bytes()).digest()
    analyzed = run(['--analyze-saved', str(detailed)])
    assert analyzed.returncode == 0, analyzed.stderr
    gap_line = next(line for line in analyzed.stdout.splitlines() if line.startswith('gap_start_s='))
    expected = ''.join(chr(b) if 32 <= b < 127 and b != 92 else
                       '\\\\' if b == 92 else f'\\x{b:02x}' for b in reason.encode())
    assert gap_line.endswith(' reason=' + expected), repr(gap_line)
    assert '\x1b' not in analyzed.stdout and '\r' not in analyzed.stdout
    assert hashlib.sha256(detailed.read_bytes()).digest() == before

for threshold in (-140, 0):
    result = run(["--headless-demo", "--spectrum-only", "--seconds", "0.1",
                  "--activity-threshold-dbfs", str(threshold)])
    assert result.returncode == 0, result.stderr
    assert value(result.stdout, "activity_threshold_dbfs") == threshold

print(f"PASS: {checked} survey/GPS/export guards, help, compact/detailed synthetic recording, 5 arbitrary-width readbacks, filtered reports, archive, privacy, source preservation, empty selections and threshold checks; no devices opened")
