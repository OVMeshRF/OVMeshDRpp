# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic evidence for the optional offline audit tool; no radio or GPS."""
import importlib.util
from pathlib import Path
import sqlite3
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("survey_audit", ROOT / "tools/audit_saved_survey.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class AuditChecks(unittest.TestCase):
    def test_mask_validation(self):
        self.assertEqual(audit.unpack_mask(bytes([0, 1, 2]), 2), bytes([1, 2]))
        self.assertEqual(audit.unpack_mask(bytes([1, 3, 0, 17, 1, 0, 0]), 4), bytes([17, 17, 17, 0]))
        for encoded, length in [(b'', 1), (b'\x01\x00\x00\x00', 1), (b'\x01\xff\xff\x01', 2),
                                (b'\x00\x01', 2), (b'\x02', 1), (b'\x01\x01', 1)]:
            with self.assertRaises(ValueError):
                audit.unpack_mask(encoded, length)

    def test_scope_rejection_before_open(self):
        with self.assertRaises(ValueError):
            audit.audit(Path('/not-an-approved-survey.sqlite'), [])

    def test_independent_counts_and_position_coverage(self):
        folder = ROOT / 'build/private-surveys'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='synthetic-audit-', dir=folder) as name:
            path = Path(name) / 'fixture.sqlite'
            db = sqlite3.connect(path)
            db.executescript('''
                PRAGMA user_version=4;
                CREATE TABLE session(center,sample_rate,complete,measurement_seconds,threshold);
                CREATE TABLE spectrum_tiles(id,first_sample,end_sample,elapsed_start,elapsed_end,fft_size,
                    frame_count,bin_count,first_center,bin_width,activity,start_lat,end_lat);
                CREATE TABLE spectrum_events(quality,start_lat,end_lat);
                CREATE TABLE coverage_gaps(elapsed_start,elapsed_end);
            ''')
            duration = 4096 / 1000000
            width = 1000000 / 4096
            db.execute('INSERT INTO session VALUES (?,?,?,?,?)', (1000000000, 1000000, 1, 8 * duration, -55))
            # Four distinct patterns: left, center, both, quiet. Repeated twice.
            for i in range(2):
                db.execute('INSERT INTO spectrum_tiles VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)',
                    (i+1, i*16384, (i+1)*16384, i*4*duration, (i+1)*4*duration, 4096, 4, 9,
                     1000000000 - 4*width, width, bytes([0, 1, 0, 16, 0, 17, 0, 0, 0]),
                     0 if i == 0 else None, 0))
            db.execute('INSERT INTO spectrum_events VALUES (?,?,?)', (8, None, 0))
            db.commit(); db.close()
            before = path.read_bytes()
            result = audit.audit(path, [('left', 1000000000 - 4.5*width, 1000000000 - 3.5*width)])
            self.assertEqual(result['frames'], 8)
            self.assertAlmostEqual(result['missing_start_position_seconds'], 4*duration)
            self.assertEqual(result['missing_end_position_seconds'], 0)
            self.assertEqual(result['event_start_positions_missing'], 1)
            self.assertAlmostEqual(result['selections'][0]['occupancy_percent'], 75)
            self.assertAlmostEqual(result['selections'][0]['outside_center_occupancy_percent'], 50)
            self.assertAlmostEqual(result['selections'][1]['occupancy_percent'], 50)
            self.assertEqual(before, path.read_bytes())
            self.assertEqual(result['schema_version'], 4)
            # The schema-5 additions must not change the independent spectrum math.
            db = sqlite3.connect(path)
            db.execute('PRAGMA user_version=5'); db.close()
            schema5 = audit.audit(path, [('left', 1000000000 - 4.5*width, 1000000000 - 3.5*width)])
            self.assertEqual(schema5['schema_version'], 5)
            for field in ('frames', 'observed_seconds', 'selections', 'event_quality'):
                self.assertEqual(result[field], schema5[field])
            # Compact storage references original fixes. Fine activity and its
            # independent union calculation must remain exactly unchanged.
            db = sqlite3.connect(path)
            db.executescript('''
                PRAGMA user_version=6;
                CREATE TABLE positions(latitude,longitude);
                INSERT INTO positions VALUES (0,0);
                CREATE TABLE survey_metrology(encoding);
                ALTER TABLE spectrum_tiles ADD COLUMN start_fix;
                ALTER TABLE spectrum_tiles ADD COLUMN end_fix;
                UPDATE spectrum_tiles SET start_fix=CASE WHEN start_lat IS NULL THEN NULL ELSE 1 END,end_fix=1;
            ''')
            db.execute('INSERT INTO survey_metrology VALUES (?)', (audit.COMPACT_ENCODING,))
            db.commit(); db.close()
            schema6 = audit.audit(path, [('left', 1000000000 - 4.5*width, 1000000000 - 3.5*width)])
            self.assertEqual(schema6['schema_version'], 6)
            for field in ('frames', 'observed_seconds', 'selections', 'event_quality',
                          'missing_start_position_seconds', 'missing_end_position_seconds'):
                self.assertEqual(result[field], schema6[field])
            db = sqlite3.connect(path)
            db.execute('UPDATE spectrum_tiles SET end_fix=999 WHERE id=1')
            db.commit(); db.close()
            with self.assertRaisesRegex(ValueError, 'referenced receiver position'):
                audit.audit(path, [])
            db = sqlite3.connect(path)
            db.execute('UPDATE spectrum_tiles SET end_fix=1')
            db.execute("UPDATE survey_metrology SET encoding='unknown'")
            db.commit(); db.close()
            with self.assertRaisesRegex(ValueError, 'encoding'):
                audit.audit(path, [])


if __name__ == '__main__':
    unittest.main()
