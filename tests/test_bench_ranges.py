#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('ranges',ROOT/'scripts/bench_ranges.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class MatrixTests(unittest.TestCase):
    def args(self,*extra):return m.parser().parse_args(['--storage-root','/scratch','--output','/results',*extra])
    def test_matrix_and_shared_geometry(self):
        rows=list(m.cases(self.args()));self.assertEqual(len(rows),24)
        self.assertEqual(len({c['name'] for c in rows}),24)
        for c in rows:
            a=c['args'];self.assertEqual(a[a.index('--container-bytes')+1],str(1024**3))
    def test_diagnostic_options_are_scoped(self):
        for c in m.cases(self.args('--split-write','--punch-before-write')):
            self.assertEqual('--split-write' in c['args'],c['access'] in ['hot-locked','hot-unlocked'])
            self.assertEqual('--punch-before-write' in c['args'],'--split-write' in c['args'])
    def test_bad_matrix_rejected(self):
        for args in [('--seconds','-1'),('--accesses','bad'),('--steps','0'),('--repeat','0'),('--container-bytes','1')]:
            with self.subTest(args=args),self.assertRaises(ValueError):list(m.cases(self.args(*args)))
if __name__=='__main__':unittest.main()
