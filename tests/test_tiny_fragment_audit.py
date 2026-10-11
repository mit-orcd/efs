#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import tempfile
import unittest

s = importlib.util.spec_from_file_location("audit", Path(__file__).resolve().parents[1] / "tools/tiny_fragment_audit.py")
a = importlib.util.module_from_spec(s)
s.loader.exec_module(a)


class AuditTests(unittest.TestCase):
    def test_path_matches_store_and_rejects_invalid_identifiers(self):
        self.assertEqual(str(a.fragment_path('/r', 1, 20650, 0, 2, 3379240209574875210)),
                         '/r/data/exports/1/0000/0000/0000/0002/0650/0/0.2.3379240209574875210')
        self.assertEqual(a.fragment_path('/r', 1, 20650, 2048, 0, 0).name, '2048.0')
        self.assertEqual(a.fragment_path('/r', 1, 20650, 2048, 0, 0).parent.name, '2')
        for ino in (-1, 1 << 64):
            with self.assertRaises(ValueError): a.fragment_path('/r', 1, ino, 0, 0, 0)
        with self.assertRaises(ValueError): a.fragment_path('/r', 1, 1, 0, 3, 0)

    def test_missing_duplicate_bad_size_and_symlink_are_not_success(self):
        with tempfile.TemporaryDirectory() as d:
            roots = [Path(d)/'a', Path(d)/'b']
            args = (roots, 1, 20650, 0, 2, 42)
            self.assertFalse(a.audit(*args)['complete'])
            p = a.fragment_path(roots[0], *args[1:]); p.parent.mkdir(parents=True)
            p.write_bytes(b'x' * 69632)
            row = a.audit(*args, expected_bytes=69632)
            self.assertTrue(row['complete'])
            self.assertEqual(row['matches'][0]['allocated_bytes'], p.stat().st_blocks * 512)
            self.assertFalse(a.audit(*args, expected_bytes=65536)['complete'])
            q = a.fragment_path(roots[1], *args[1:]); q.parent.mkdir(parents=True)
            q.write_bytes(b'bad')
            self.assertFalse(a.audit(*args)['complete'])
            q.unlink(); q.symlink_to(p)
            self.assertFalse(a.audit(*args)['complete'])
            with self.assertRaises(ValueError): a.audit([roots[0], roots[0]], *args[1:])


if __name__ == '__main__': unittest.main()
