#!/usr/bin/env python3
"""Negative controls for the generated K register: gaps, duplicates, bad states, stale table."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts/check_k_register.py'
spec = importlib.util.spec_from_file_location('check_k_register', SCRIPT)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def row(n, state='FIXED', title='item'):
    return f'# K{n}: {title}\n\n- State: {state}\n- Blocks alpha: no\n- Kind: kind\n\n## Item\n\nx\n'


def run(path, *args):
    return subprocess.run([sys.executable, str(SCRIPT), str(path), *args], capture_output=True, text=True)


class RegisterTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix='efs-k-register-')
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def tree(self, rows=((1, 'FIXED'), (2, 'OPEN'))):
        """Write row files and a checklist whose generated block is current."""
        (self.tmp / 'k').mkdir(exist_ok=True)
        for n, state in rows:
            (self.tmp / 'k' / f'K{n:03d}.md').write_text(row(n, state))
        path = self.tmp / 'checklist.md'
        path.write_text(f'intro\n{checker.BEGIN}\n{checker.END}\n')
        run(path, '--write')  # fails for broken trees; the test then checks the diagnostics
        return path

    def test_pass_and_counts(self):
        path = self.tree(tuple(enumerate(checker.STATES, 1)))
        p = run(path)
        self.assertEqual(p.returncode, 0, p.stdout)
        self.assertIn('PASS K register: 6 rows', p.stdout)
        self.assertIn('Current counts: 1 FIXED / 1 VERIFIED / 1 VERIFY / 1 CLOSED / 1 DECIDED / 1 OPEN (6 rows).',
                      path.read_text())

    def test_missing_number(self):
        p = run(self.tree(((1, 'FIXED'), (3, 'OPEN'))))
        self.assertEqual(p.returncode, 1)
        self.assertIn('missing K2', p.stdout)

    def test_duplicate_number(self):
        path = self.tree()
        (self.tmp / 'k/K0002.md').write_text(row(2))
        p = run(path)
        self.assertEqual(p.returncode, 1)
        self.assertIn('K<NNN>.md', p.stdout)

    def test_bad_state(self):
        path = self.tree()
        for state in ('FIX', 'Fixed', '**FIXED**', 'FIXEDISH', ''):
            with self.subTest(state=state):
                (self.tmp / 'k/K001.md').write_text(row(1, state))
                p = run(path)
                self.assertEqual(p.returncode, 1)
                self.assertIn('State must be one of', p.stdout)

    def test_stale_table_and_write(self):
        path = self.tree()
        (self.tmp / 'k/K002.md').write_text(row(2, 'VERIFY'))
        p = run(path)
        self.assertEqual(p.returncode, 1)
        self.assertIn('stale', p.stdout)
        self.assertEqual(run(path, '--write').returncode, 0)
        self.assertEqual(run(path).returncode, 0)

    def test_pipe_in_title_stays_one_cell(self):
        self.assertEqual(checker.generate({1: ('a|b', 'k|k', 'OPEN', 'no')}).splitlines()[2].count('|'), 7)

    def test_json(self):
        p = run(self.tree(), '--json')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('"OPEN": 1', p.stdout)


if __name__ == '__main__':
    unittest.main()
