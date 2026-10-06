#!/usr/bin/env python3
"""Check profiling isolation, failure evidence, and safe scratch ownership."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import subprocess
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('bench_profile', ROOT / 'scripts/bench_profile.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class ProfileTests(unittest.TestCase):
    def test_remote_prime_and_matrix(self):
        a = bench.parser().parse_args(['--modes', 'all', '--seed', 'seed:123', '--threads', '1',
                                      '--hash-sizes', '64K', '--qds', '1', '--writers', '0'])
        cases = bench.cases_for(a, 4)
        self.assertLess([c['name'] for c in cases].index('store-prime'), [c['name'] for c in cases].index('read'))
        self.assertEqual(len(cases), 10)
        a.seed = None
        with self.assertRaises(ValueError):
            bench.cases_for(a, 4)

    def test_timeout_stops_descendant_work(self):
        with tempfile.TemporaryDirectory() as root:
            flag = Path(root) / 'child-survived'
            child = f"import time; from pathlib import Path; time.sleep(.4); Path({str(flag)!r}).write_text('bad')"
            parent = f"import subprocess,time; subprocess.Popen([{sys.executable!r}, '-c', {child!r}]); time.sleep(30)"
            with self.assertRaises(subprocess.TimeoutExpired):
                bench.execute([sys.executable, '-c', parent], Path(root), 'timeout', .15)
            time.sleep(.45)
            self.assertFalse(flag.exists())

    def test_metrics_and_symbols(self):
        self.assertFalse(bench.valid_metrics([]))
        self.assertFalse(bench.valid_metrics([{'ops': '0'}]))
        self.assertFalse(bench.valid_metrics([{'chunks_ok': '2', 'chunks_fail': '1'}]))
        self.assertTrue(bench.valid_metrics([{'ops': '10', 'errors': '0'}]))
        with tempfile.TemporaryDirectory() as root:
            p = Path(root) / 'symbols'
            p.write_text('# header\n55.00%;efs-bench;[.] blake3_compress\n10.00%;libc;[.] memcpy\n')
            self.assertEqual(bench.profile_rows(p)[0]['symbol'], 'blake3_compress')

    def run_harness(self, fail=False, empty=False):
        with tempfile.TemporaryDirectory(prefix='profile test ') as tmp:
            root = Path(tmp)
            tools = root / 'tools'
            tools.mkdir()
            binary = root / 'efs-bench'
            binary.write_text('''#!/usr/bin/env python3
import sys
if '--version' in sys.argv: print('fake version'); sys.exit(0)
print('BENCH_OK kind=data rw=write ops=4 GiB_s=1 errors=0')
print('BENCH_OK kind=data rw=read ops=4 GiB_s=2 errors=0')
''')
            binary.chmod(0o755)
            perf = tools / 'perf'
            perf.write_text('''#!/usr/bin/env python3
import os, pathlib, subprocess, sys
args=sys.argv[1:]
if args[0]=='record':
    pathlib.Path(args[args.index('-o')+1]).write_bytes(b'fake profile')
    sys.exit(subprocess.call(args[args.index('--')+1:]))
if args[0]=='report':
    if os.environ.get('FAIL_REPORT'): sys.exit(1)
    if not os.environ.get('EMPTY_REPORT'):
        print('75.00%;efs-bench;[.] efs_hash')
        print('25.00%;libc.so;[.] memcpy')
    sys.exit(0)
if args[0]=='annotate': print('source.c:10 fake annotation'); sys.exit(0)
sys.exit(1)
''')
            perf.chmod(0o755)
            trace = tools / 'strace'
            trace.write_text('''#!/usr/bin/env python3
import pathlib, subprocess, sys
args=sys.argv[1:]; i=args.index('-o')
pathlib.Path(args[i+1]).write_text('syscall summary')
sys.exit(subprocess.call(args[i+2:]))
''')
            trace.chmod(0o755)
            scratch = root / 'caller storage'
            scratch.mkdir()
            sentinel = scratch / 'keep'
            sentinel.write_text('do not delete')
            output = root / 'output'
            env = dict(PATH=str(tools) + os.pathsep + os.environ['PATH'])
            if fail:
                env['FAIL_REPORT'] = '1'
            if empty:
                env['EMPTY_REPORT'] = '1'
            with patch.dict(os.environ, env), patch.object(bench.sys, 'platform', 'linux'):
                rc = bench.main(['--binary', str(binary), '--output', str(output), '--storage-root', str(scratch),
                                 '--modes', 'data', '--qds', '1', '--writers', '0', '--skip-ceiling', '--strace', '--time', '.01'])
            self.assertEqual(rc, 1 if fail or empty else 0)
            self.assertEqual(list(scratch.iterdir()), [sentinel])
            result = json.loads((output / 'results.json').read_text())[0]
            basecmd = result['runs']['baseline']['command']
            perfcmd = result['runs']['perf']['command']
            self.assertNotIn('--perf', basecmd)
            self.assertNotIn('strace', ' '.join(perfcmd))
            self.assertNotEqual(basecmd, perfcmd)  # fresh scratch for each stage
            analysis = (output / 'ANALYSIS.md').read_text()
            self.assertIn('FAIL' if fail or empty else 'efs_hash', analysis)
            if not fail and not empty:
                self.assertTrue((output / result['name'] / 'strace' / 'summary.txt').is_file())
                self.assertIn('source.c:10', (output / result['name'] / 'perf' / 'annotate-1.stdout').read_text())
            with self.assertRaises(FileExistsError), patch.object(bench.sys, 'platform', 'linux'), patch.dict(os.environ, env):
                bench.main(['--binary', str(binary), '--output', str(output), '--modes', 'meta', '--no-perf'])

    def test_full_profile_and_optional_separate_trace(self):
        self.run_harness()

    def test_report_failure_is_not_pass(self):
        self.run_harness(fail=True)

    def test_empty_profile_is_not_pass(self):
        self.run_harness(empty=True)


if __name__ == '__main__':
    unittest.main()
