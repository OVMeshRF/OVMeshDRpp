#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent, read-only schema-4/5/6 spectrum measurement audit; Python standard library only.

Prints aggregate RF/coverage statistics, never coordinates, notes or payloads.
Discovery evidence is not used to calculate spectrum occupancy.
Optional CLI comparison uses only this checkout's built application. No devices.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import re
import sqlite3
import subprocess

COMPACT_ENCODING = (
    "little-endian signed int16 centidB; activity codec 0=bytes,1=u16le count+byte; frame-major LSB-first"
    "; compact power: frame-weighted linear mean and per-bin maximum over <=1s contiguous blocks;"
    " tile activity unchanged; endpoint fixes reference positions.rowid"
)


def unpack_mask(blob, expected):
    if not blob or not 0 < expected <= 65536:
        raise ValueError("Invalid activity dimensions")
    if blob[0] == 0:
        data = blob[1:]
    elif blob[0] == 1 and (len(blob) - 1) % 3 == 0:
        data = bytearray()
        for offset in range(1, len(blob), 3):
            count = int.from_bytes(blob[offset:offset + 2], "little")
            if count == 0 or len(data) + count > expected:
                raise ValueError("Invalid activity run")
            data.extend(bytes([blob[offset + 2]]) * count)
    else:
        raise ValueError("Unsupported activity encoding")
    if len(data) != expected:
        raise ValueError("Activity length mismatch")
    return data


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def audit(path, ranges, compare_cli=False):
    # This developer tool is intentionally restricted to the approved private
    # test-data directory. Reject aliases/traversal before reading a target.
    allowed = Path(__file__).resolve().parent.parent / "build/private-surveys"
    if not path.is_absolute() or ".." in path.parts or not path.is_relative_to(allowed):
        raise ValueError("Use a saved survey inside this checkout's build/private-surveys directory")
    if any(parent.is_symlink() for parent in (path, *path.parents)) or not path.is_file():
        raise ValueError("Use a regular local survey file without symbolic aliases")
    before = digest(path)
    wal = Path(str(path) + "-wal")
    if wal.is_symlink():
        raise ValueError("A survey WAL must not be a symbolic alias")
    wal_before = digest(wal) if wal.exists() else None
    connection = sqlite3.connect(path.as_uri() + "?mode=ro")
    try:
        connection.execute("PRAGMA query_only=ON")
        connection.execute("PRAGMA trusted_schema=OFF")
        connection.execute("BEGIN")
        schema_version = connection.execute("PRAGMA user_version").fetchone()[0]
        if schema_version not in (4, 5, 6):
            raise ValueError("This spectrum audit requires schema 4, 5 or 6")
        if schema_version == 6:
            encoding = connection.execute("SELECT encoding FROM survey_metrology").fetchall()
            if encoding != [(COMPACT_ENCODING,)]:
                raise ValueError("Unsupported compact measurement encoding")
            dangling = connection.execute(
                "SELECT count(*) FROM spectrum_tiles t LEFT JOIN positions a ON a.rowid=t.start_fix "
                "LEFT JOIN positions b ON b.rowid=t.end_fix "
                "WHERE (t.start_fix IS NOT NULL AND a.rowid IS NULL) "
                "OR (t.end_fix IS NOT NULL AND b.rowid IS NULL)").fetchone()[0]
            if dangling:
                raise ValueError("Missing referenced receiver position")
        center, rate, complete, saved_seconds, threshold = connection.execute(
            "SELECT center,sample_rate,complete,measurement_seconds,threshold FROM session").fetchone()
        if not complete:
            raise ValueError("Audit a finalized saved session, not a running/incomplete recording")
        first, width, bins = connection.execute(
            "SELECT first_center,bin_width,bin_count FROM spectrum_tiles ORDER BY id LIMIT 1").fetchone()
        if not 1 <= bins <= 4096 or not rate > 0 or not width > 0:
            raise ValueError("Invalid recorded grid")
        ranges = [("full_range", first - width / 2, first + (bins - .5) * width)] + ranges
        guard = sum(1 << i for i in range(bins) if abs(first + i * width - center) <= 2 * width + 1e-5)
        selections = []
        for name, low, high in ranges:
            if not math.isfinite(low) or not math.isfinite(high) or not 0 <= low < high:
                raise ValueError("Invalid frequency selection")
            indexes = [i for i in range(bins) if first + (i + .5) * width > low and first + (i - .5) * width < high]
            if not indexes:
                raise ValueError("Selection has no recorded bins")
            selections.append(dict(name=name, requested_lower_hz=low, requested_upper_hz=high,
                lower_hz=first + (indexes[0] - .5) * width, upper_hz=first + (indexes[-1] + .5) * width,
                mask=sum(1 << i for i in indexes), busy_frames=0, center_frames=0, outside_frames=0))
        frames = tiles = missing_start = missing_end = discontinuities = 0
        previous_sample = previous_elapsed = None
        elapsed_sum = 0.0
        position_columns = "start_fix IS NULL,end_fix IS NULL" if schema_version == 6 else "start_lat IS NULL,end_lat IS NULL"
        sql = ("SELECT first_sample,end_sample,elapsed_start,elapsed_end,fft_size,frame_count,bin_count,"
               "first_center,bin_width,activity," + position_columns + " FROM spectrum_tiles ORDER BY id")
        for start, end, elapsed_start, elapsed_end, fft, count, n, f, w, blob, no_start, no_end in connection.execute(sql):
            if fft != 4096 or n != bins or f != first or w != width or not 1 <= count <= 128 or end - start != count * fft:
                raise ValueError("Inconsistent FFT/grid metadata")
            if not math.isclose(elapsed_end - elapsed_start, count * fft / rate, abs_tol=1e-8):
                raise ValueError("Sample and elapsed durations disagree")
            if previous_sample is not None:
                if start < previous_sample or elapsed_start < previous_elapsed - 1e-8:
                    raise ValueError("Overlapping or reversed observations")
                discontinuities += start != previous_sample or abs(elapsed_start - previous_elapsed) > 1e-8
            previous_sample, previous_elapsed = end, elapsed_end
            stride = (bins + 7) // 8
            data = unpack_mask(blob, stride * count)
            # Count equal frame patterns, then use arbitrary-precision integer AND
            # rather than the application's byte-mask union implementation.
            patterns = Counter(bytes(data[i:i + stride]) for i in range(0, len(data), stride))
            for pattern, repeats in patterns.items():
                activity = int.from_bytes(pattern, "little")
                if activity >> bins:
                    raise ValueError("Activity outside recorded bins")
                for selection in selections:
                    selected = activity & selection["mask"]
                    selection["busy_frames"] += repeats * bool(selected)
                    selection["center_frames"] += repeats * bool(selected & guard)
                    selection["outside_frames"] += repeats * bool(selected & ~guard)
            frames += count
            tiles += 1
            missing_start += count * no_start
            missing_end += count * no_end
            elapsed_sum += elapsed_end - elapsed_start
        frame_seconds = 4096 / rate
        observed = frames * frame_seconds
        if not math.isclose(observed, elapsed_sum, abs_tol=1e-7) or not math.isclose(observed, saved_seconds, abs_tol=1e-7):
            raise ValueError("Session, tile and FFT observation totals disagree")
        events, start_missing, end_missing = connection.execute(
            "SELECT count(*),coalesce(sum(start_lat IS NULL),0),coalesce(sum(end_lat IS NULL),0) FROM spectrum_events").fetchone()
        quality = [dict(flags=q, events=n) for q, n in connection.execute(
            "SELECT quality,count(*) FROM spectrum_events GROUP BY quality ORDER BY quality")]
        gaps, gap_seconds = connection.execute(
            "SELECT count(*),coalesce(sum(elapsed_end-elapsed_start),0) FROM coverage_gaps").fetchone()
        result = dict(schema_version=schema_version, sha256=before, tiles=tiles, frames=frames, observed_seconds=observed,
            threshold_dbfs_per_bin=threshold, missing_start_position_seconds=missing_start * frame_seconds,
            missing_end_position_seconds=missing_end * frame_seconds, tile_discontinuities=discontinuities,
            recorded_gap_rows=gaps, summed_gap_seconds=gap_seconds,
            events=events, event_start_positions_missing=start_missing, event_end_positions_missing=end_missing,
            event_quality=quality, selections=[])
        for selection in selections:
            mask = selection.pop("mask")
            for kind in ("busy", "center", "outside"):
                selection[kind + "_seconds"] = selection.pop(kind + "_frames") * frame_seconds
            selection["occupancy_percent"] = 100 * selection["busy_seconds"] / observed if observed else None
            selection["outside_center_occupancy_percent"] = (
                100 * selection["outside_seconds"] / observed if observed and mask & ~guard else None)
            if compare_cli:
                cli = Path(__file__).resolve().parent.parent / "build/native/ovmesh-cli"
                command = [str(cli), "--analyze-saved", str(path), "--frequency-range",
                           f'{selection["requested_lower_hz"]},{selection["requested_upper_hz"]}']
                output = subprocess.check_output(command, text=True, timeout=120)
                fields = dict(re.findall(r"([a-z_]+)=(-?\d+(?:\.\d+)?)", output))
                for field, expected in (("observed_s", observed), ("busy_s", selection["busy_seconds"]),
                    ("center_busy_s", selection["center_seconds"]), ("outside_center_busy_s", selection["outside_seconds"]),
                    ("missing_start_position_s", result["missing_start_position_seconds"]),
                    ("missing_end_position_s", result["missing_end_position_seconds"]),
                    ("covered_lower_hz", selection["lower_hz"]), ("covered_upper_hz", selection["upper_hz"])):
                    if not math.isclose(float(fields[field]), expected, rel_tol=0, abs_tol=2e-6):
                        raise ValueError(f'CLI mismatch for {selection["name"]}: {field}')
                selection["cli_matches"] = True
            result["selections"].append(selection)
        return result
    finally:
        connection.close()
        if digest(path) != before or (digest(wal) if wal.exists() else None) != wal_before:
            raise ValueError("Source database changed during audit; discard this comparison")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("survey", type=Path)
    parser.add_argument("--range", nargs=3, action="append", metavar=("NAME", "LOW_HZ", "HIGH_HZ"), default=[])
    parser.add_argument("--compare-cli", action="store_true")
    args = parser.parse_args()
    print(json.dumps(audit(args.survey, [(name, float(low), float(high)) for name, low, high in args.range], args.compare_cli), indent=2))
