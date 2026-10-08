#!/usr/bin/env python3
"""Check matrix bounds and that container-only knobs leave the file reference alone."""
import importlib.util
import tempfile
import subprocess
import sys
import json
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
    def test_file_body_batching_only_applies_to_files(self):
        a=m.parser().parse_args(['--storage-root','/scratch','--file-batch-sync'])
        for c in m.cases(a):self.assertEqual('--file-batch-sync' in c['args'],c['layout']=='files')
        a.persist='each'
        with self.assertRaises(ValueError):list(m.cases(a))

    def test_timed_run_has_hard_cap(self):
        a=m.parser().parse_args(["--storage-root","/scratch","--seconds","120"])
        for c in m.cases(a):
            args=c["args"];self.assertEqual(args[args.index("--steps")+1],"10000000")
            self.assertEqual(args[args.index("--seconds")+1],"120")

    def test_comparison_alternates_matched_arguments(self):
        a=m.parser().parse_args(['--storage-root','/a','--storage-root','/b','--compare',
            '--qds','2','--repeat','2','--seed','99','--hot-access-percent','90'])
        c=list(m.cases(a));self.assertEqual([v['layout'] for v in c[:6]],['files','punch','compact','compact','punch','files'])
        for v in c:
            self.assertIn('99',v['args']);self.assertIn('--hot-access-percent',v['args'])
        a.qds=[1]
        with self.assertRaises(ValueError):list(m.cases(a))

    def test_locality_and_seed_bounds(self):
        for args in [['--seed','0'],['--seed',str(2**64)],['--hot-set-percent','100'],['--hot-access-percent','101'],['--compare','--layouts','files,files']]:
            with self.subTest(args=args),self.assertRaises(ValueError):
                list(m.cases(m.parser().parse_args(['--storage-root','/scratch',*args])))

    def test_runner_checks_trace_and_writes_comparison(self):
        for mismatch in (False,True):
            with self.subTest(mismatch=mismatch),tempfile.TemporaryDirectory() as temp:
                root=Path(temp);scratch=root/'scratch';scratch.mkdir()
                binary=root/'fake';binary.write_text("#!/usr/bin/env python3\nimport sys\na=sys.argv\nlayout=a[a.index('--layout')+1]\nprint('BENCH_OK errors=0 verified_objects=4 live_objects=4 reads=5 writes=5 ops_s=10')\nprint('WORKLOAD trace='+('bad' if "+repr(mismatch)+" and layout=='punch' else 'same')+' cpu_us_per_op=1')\nprint('TARGET target=0 workers=1 ops_s=10 allocated_after=0')\n")
                binary.chmod(0o755)
                out=root/'results'
                result=subprocess.run([sys.executable,str(ROOT/'scripts/bench_churn.py'),
                    '--storage-root',str(scratch),'--output',str(out),'--binary',str(binary),
                    '--compare','--layouts','files,punch','--occupancies','80',
                    '--payload-sizes','4096','--io-modes','buffered','--repeat','2'],capture_output=True,text=True)
                self.assertEqual(result.returncode,int(mismatch),result.stdout+result.stderr)
                rows=json.loads((out/'results.json').read_text())
                self.assertEqual(rows[-1]['status'],'FAIL' if mismatch else 'PASS')
                self.assertTrue((out/'COMPARISON.md').is_file())
                self.assertTrue((out/'sources.zip').is_file())
                if not mismatch:self.assertEqual(list(scratch.iterdir()),[])

    def test_invalid_model_is_rejected(self):
        for args in [['--occupancies','100'],['--capacity-per-worker','1000'],['--payload-sizes','1000'],['--read-percent','0'],['--steps','0'],['--layouts','production']]:
            with self.subTest(args=args),self.assertRaises(ValueError):
                list(m.cases(m.parser().parse_args(['--storage-root','/scratch',*args])))
if __name__=='__main__':unittest.main()
