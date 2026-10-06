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
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('bench_profile', ROOT / 'scripts/bench_profile.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class ProfileTests(unittest.TestCase):
    def test_remote_prime_and_matrix(self):
        a = bench.parser().parse_args(['--modes', 'all', '--seed', 'seed:123', '--threads', '1',
                                      '--hash-sizes', '64K', '--io-sizes', '64K', '--qds', '1', '--writers', '0'])
        cases = bench.cases_for(a, 4)
        self.assertLess([c['name'] for c in cases].index('store-prime'), [c['name'] for c in cases].index('read'))
        self.assertEqual(len(cases), 18)
        a.seed = None
        with self.assertRaises(ValueError):
            bench.cases_for(a, 4)

    def test_raw_io_pairs_use_identical_geometry(self):
        a = bench.parser().parse_args(['--modes', 'io,io-blake3', '--io-sizes', '64K', '--qds', '2', '--data-size', '1M'])
        cases = bench.cases_for(a, 4)
        self.assertEqual(len(cases), 8)
        for c in cases[:4]:
            partner = next(p for p in cases if p['name'] == c['name'].replace('io-', 'io-blake3-', 1))
            self.assertEqual(c['args'][2:], partner['args'][2:])
        with self.assertRaises(ValueError):
            a.data_size = [4096]
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

    def test_failed_metrics_and_unresolved_samples(self):
        with tempfile.TemporaryDirectory() as root:
            p = Path(root) / 'metrics'
            p.write_text('BENCH_FAIL kind=io ops=10 errors=0 GiB_s=999999\n')
            self.assertFalse(bench.valid_metrics(bench.metric_rows(p)))
        self.assertFalse(bench.valid_metrics([{'ops': '10', 'idle_workers': '1'}]))
        self.assertEqual(bench.category({'symbol': 'copy_page_from_iter_atomic', 'dso': '[kernel.kallsyms]'}), 'memory copies/fills')
        self.assertEqual(bench.category({'symbol': 'iommu_v1_map_pages', 'dso': '[kernel.kallsyms]'}), 'page pinning/IOMMU')
        self.assertEqual(bench.category({'symbol': '0x123', 'dso': '[vdso]'}), 'time/vDSO')
        self.assertEqual(bench.category({'symbol': '0x123', 'dso': '[kernel.kallsyms]'}), 'unresolved samples')

    def test_analysis_excludes_bad_baseline_but_retains_good_perf_failure(self):
        with tempfile.TemporaryDirectory() as root:
            output = Path(root)
            manifest = dict(host='test', event='cycles', binary_sha256='test', commit='test', cases=[], seed=None)
            good = dict(name='io-good', status='FAIL(perf)', metrics=[dict(kind='io', ops='10', errors='0', GiB_s='1.234', rw='read')],
                        runs={'baseline': dict(returncode=0, metrics_valid=True)})
            bad = dict(name='io-bad', status='FAIL(baseline)', metrics=[dict(kind='io', ops='10', errors='1', GiB_s='999999')],
                       runs={'baseline': dict(returncode=1, metrics_valid=False)})
            (output / 'manifest.json').write_text(json.dumps(manifest))
            (output / 'results.json').write_text(json.dumps([good, bad]))
            profile = output / 'io-good' / 'perf'
            profile.mkdir(parents=True)
            (profile / 'symbols.stdout').write_text("# event 'cycles:u'\n45.00%;[kernel.kallsyms];[k] 0x123\n")
            bench.analyze(output)
            analysis = (output / 'ANALYSIS.md').read_text()
            self.assertNotIn('999999', analysis)
            self.assertIn('best observed 1.234', analysis)
            self.assertIn('unresolved symbols', analysis)
            self.assertIn('`cycles:u`', analysis)

    def test_bounded_parallel_reports_include_failed_workloads(self):
        with tempfile.TemporaryDirectory() as root:
            output = Path(root)
            results = []
            for i in range(5):
                d = output / str(i) / 'perf'; d.mkdir(parents=True)
                (d / 'perf.data').write_bytes(b'profile')
                results.append(dict(name=str(i), status='FAIL (perf)' if i == 4 else 'REPORTS_PENDING',
                                    runs={'perf': {'returncode': 1 if i == 4 else 0}}))
            lock = threading.Lock()
            active = peak = 0
            def fake_report(perf, directory, args):
                nonlocal active, peak
                with lock:
                    active += 1; peak = max(peak, active)
                time.sleep(.03)
                with lock:
                    active -= 1
                return True, []
            a = bench.parser().parse_args(['--report-jobs', '2'])
            with patch.object(bench, 'reports', fake_report):
                self.assertTrue(bench.report_all('perf', output, results, a))
            self.assertEqual(peak, 2)
            self.assertTrue(all(r['runs']['perf']['reports_state'] == 'COMPLETE' for r in results))
            self.assertEqual(results[-1]['status'], 'FAIL (perf)')
            self.assertTrue(all(r['status'] == 'PASS' for r in results[:-1]))

    def test_missing_profile_is_a_report_failure(self):
        with tempfile.TemporaryDirectory() as root:
            results = [dict(name='missing', status='PASS', runs={'perf': {'returncode': 0}})]
            a = bench.parser().parse_args([])
            self.assertFalse(bench.report_all('perf', Path(root), results, a))
            self.assertEqual(results[0]['status'], 'FAIL (reports)')
            self.assertEqual(results[0]['runs']['perf']['reports_error'], 'perf.data missing')

    def test_reports_only_never_plans_or_executes_workloads(self):
        with tempfile.TemporaryDirectory() as root:
            output = Path(root)
            (output / 'manifest.json').write_text(json.dumps(dict(host='test', event='cycles', binary_sha256='test', commit='test', cases=[], seed=None)))
            (output / 'results.json').write_text('[]')
            with patch.object(bench.sys, 'platform', 'linux'), patch.object(bench.shutil, 'which', return_value='perf'), \
                 patch.object(bench, 'report_all', return_value=True) as report, \
                 patch.object(bench, 'cases_for', side_effect=AssertionError('workload planning')):
                self.assertEqual(bench.main(['--reports-only', str(output)]), 0)
            report.assert_called_once()

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
            for name in ['flat.txt', 'by_thread.txt', 'callers.txt']:
                self.assertTrue((output / result['name'] / 'perf' / name).is_file())
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
