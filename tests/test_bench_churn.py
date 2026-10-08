#!/usr/bin/env python3
"""Check matrix bounds and that container-only knobs leave the file reference alone."""
import importlib.util
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('bench_churn',ROOT/'scripts/bench_churn.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class MatrixTests(unittest.TestCase):
    def test_matched_matrix(self):
        a=m.parser().parse_args(['--storage-root','/scratch'])
        cases=list(m.cases(a));self.assertEqual(len(cases),18)
        self.assertEqual(len({c['name'] for c in cases}),18)
        self.assertEqual({c['occupancy'] for c in cases},{50,80,95})
        for c in cases:
            args=c['args'];self.assertEqual(args[args.index('--capacity-bytes')+1],str(2*1024**3))
            self.assertEqual(args[args.index('--container-bytes')+1],str(1024**3))
            self.assertNotIn('--storage',args)
    def test_container_knobs_only(self):
        a=m.parser().parse_args(['--storage-root','/scratch','--preallocate','--extent-hint','1048576','--filestreams'])
        for c in m.cases(a):
            self.assertEqual('--preallocate' in c['args'],c['layout']!='files')
            self.assertEqual('--extent-hint' in c['args'],c['layout']!='files')
            self.assertIn('--filestreams',c['args'])
    def test_invalid_model_is_rejected(self):
        for args in [['--occupancies','100'],['--capacity-per-worker','1000'],['--payload-sizes','1000'],['--read-percent','0'],['--steps','0'],['--layouts','production']]:
            with self.subTest(args=args),self.assertRaises(ValueError):
                list(m.cases(m.parser().parse_args(['--storage-root','/scratch',*args])))
if __name__=='__main__':unittest.main()
