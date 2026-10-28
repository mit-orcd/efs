#!/usr/bin/env python3
"""Negative controls for K-register structure and canonical states."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('check_k_register', ROOT / 'scripts/check_k_register.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def document(rows=((1, 'FIXED'), (2, 'OPEN'))):
    a = '| ID | Item | Kind | Disposition |\n| --- | --- | --- | --- |\n'
    b = '\n### Recheck\n\n| Row | Finding on devel |\n| --- | --- |\n'
    for number, state in rows:
        a += f'| K{number} | item | kind | {state}, scoped; history **OPEN** |\n'
        b += f'| K{number} | {state}, history FIX: old request |\n'
    return a + b


class RegisterTests(unittest.TestCase):
    def test_all_canonical_states_and_history(self):
        text = document(tuple(enumerate(checker.STATES, 1)))
        errors, tables = checker.check(text)
        self.assertEqual(errors, [])
        self.assertEqual(len(tables['register']), len(checker.STATES))

    def test_bad_prefix_does_not_hide_behind_later_state(self):
        for state in ('FIX', 'Fixed', '**FIXED**', 'FIXEDISH', 'PASS', 'OPENING'):
            with self.subTest(state=state):
                errors, _ = checker.check(document(((1, state),)))
                self.assertTrue(any('canonical word' in e for e in errors))

    def test_duplicate_in_either_table(self):
        for row in ('| K1 | duplicate | kind | FIXED |', '| K1 | FIXED |'):
            text = document()
            if 'kind' in row:
                text = text.replace('\n\n### Recheck', '\n' + row + '\n\n### Recheck')
            else:
                text += row + '\n'
            errors, _ = checker.check(text)
            self.assertTrue(any('duplicate K1' in e for e in errors), errors)

    def test_missing_in_one_or_both_tables(self):
        for text in (document().replace('| K2 | OPEN, history FIX: old request |\n', ''),
                     document(((1, 'FIXED'), (3, 'VERIFY')))):
            errors, _ = checker.check(text)
            self.assertTrue(any('missing K2' in e for e in errors), errors)

    def test_missing_table_and_grouped_id(self):
        errors, _ = checker.check(document().split('### Recheck')[0])
        self.assertTrue(any('empty recheck' in e for e in errors))
        errors, _ = checker.check(document().replace('| K2 | OPEN', '| K2, K3 | OPEN'))
        self.assertTrue(any('one K number' in e for e in errors))

    def test_disagreement(self):
        errors, _ = checker.check(document().replace('| K2 | OPEN', '| K2 | VERIFY'))
        self.assertTrue(any('states disagree' in e for e in errors))

    def test_cli_exit_and_json(self):
        with tempfile.TemporaryDirectory(prefix='efs-k-register-') as tmp:
            path = Path(tmp) / 'checklist.md'
            path.write_text(document())
            p = subprocess.run([sys.executable, str(ROOT / 'scripts/check_k_register.py'), str(path), '--json'], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn('"OPEN": 1', p.stdout)
            path.write_text(document(((1, 'PASS'),)))
            p = subprocess.run([sys.executable, str(ROOT / 'scripts/check_k_register.py'), str(path)], capture_output=True, text=True)
            self.assertEqual(p.returncode, 1)
            self.assertIn('canonical word', p.stdout)


if __name__ == '__main__':
    unittest.main()
