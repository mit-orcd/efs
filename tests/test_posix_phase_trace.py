#!/usr/bin/env python3
"""Actual-case phase observer: quiet default, unchanged shape and failed observer isolation."""
from pathlib import Path
import ast, hashlib, importlib.util, json, os, socket, subprocess, sys, tempfile, threading, time
root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('phase_suite', root/'tests/posix/posix_suite.py')
suite = importlib.util.module_from_spec(spec); spec.loader.exec_module(suite)
tree = ast.parse((root/'tests/live/gc_reclamation.py').read_text())
node = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == 'PosixPhaseCollector')
space = dict(Path=Path, threading=threading, tempfile=tempfile, socket=socket, time=time, json=json)
exec(compile(ast.Module(body=[node], type_ignores=[]), '<actual-collector>', 'exec'), space)
Collector = space['PosixPhaseCollector']
# Strip only diagnostics and compare the actual case operations with its baseline.
def shape(source):
    module = ast.parse(source)
    fn = next(n for n in module.body if isinstance(n, ast.FunctionDef) and n.name == 'dir_deep_nesting')
    fn.body = [n for n in fn.body if not (isinstance(n, ast.Expr) and isinstance(n.value, ast.Call)
               and isinstance(n.value.func, ast.Name) and n.value.func.id == '_deep_phase')]
    return ast.dump(fn, include_attributes=False)
# Canonical AST of actual b239dc7d case, excluding only observer calls.
assert hashlib.sha256(shape((root/'tests/posix/posix_suite.py').read_text()).encode()).hexdigest() == '1cdb1d66e21f56f3a1e03b878d8cfef6807168c7d04c026f70823b840d2357e7'
saved = dict(os.environ)
try:
    os.environ.pop('EFS_POSIX_PHASE_SOCKET', None)
    with tempfile.TemporaryDirectory(prefix='efs-phase-check-', dir='/tmp') as temp:
        work = Path(temp)
        with Collector(work, False):
            assert suite.invoke(suite.dir_deep_nesting, str(work/'quiet')) == ('PASS', '')
        assert not (work/'posix-case-phases.log').exists()
        with Collector(work, True) as observer:
            os.environ.update(observer.environment(os.environ, 3))
            assert suite.invoke(suite.dir_deep_nesting, str(work/'traced')) == ('PASS', '')
        lines = (work/'posix-case-phases.log').read_text().splitlines()
        events = [dict(x.split('=', 1) for x in line.split()) for line in lines]
        assert [e['event'] for e in events] == ['setup', 'setup-complete', 'create',
                'leaf-write', 'leaf-read', 'recursive-cleanup', 'absence-check', 'case-complete']
        assert all(e['client'] == '3' and int(e['pid']) == os.getpid() for e in events)
        times = [int(e['mono_ns']) for e in events]; assert times == sorted(times)
        summary = json.loads((work/'posix-case-phases-summary.json').read_text())
        assert summary['received'] == summary['retained'] == 8 and not summary['errors']
        # No receiver, and a filled receiver queue, must never block or affect case bytes.
        os.environ['EFS_POSIX_PHASE_SOCKET'] = str(work/'absent')
        assert suite.invoke(suite.dir_deep_nesting, str(work/'absent-sink')) == ('PASS', '')
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as full:
            full.bind(str(work/'full'))
            os.environ['EFS_POSIX_PHASE_SOCKET'] = str(work/'full')
            start = time.monotonic()
            for _ in range(200): suite._deep_phase('queue-test')
            assert time.monotonic() - start < 2
            assert suite.invoke(suite.dir_deep_nesting, str(work/'full-sink')) == ('PASS', '')
        # Controlled leaf failure leaves its correct in-flight boundary; status preserved.
        with Collector(work, True) as observer:
            os.environ.update(observer.environment(os.environ, 4))
            original = suite.wr
            def failed_write(*args): raise OSError(5, 'controlled leaf IO')
            suite.wr = failed_write
            try:
                status, detail = suite.invoke(suite.dir_deep_nesting, str(work/'failed'))
                assert status == 'FAIL' and 'controlled leaf IO' in detail
            finally: suite.wr = original
        assert 'event=leaf-write ' in (work/'posix-case-phases.log').read_text().splitlines()[-1]
        assert not observer.path.exists() and not observer.thread.is_alive()
        with Collector(work, True) as observer:
            environment = observer.environment(os.environ, 6)
            result_path = work/'worker-result'
            result = subprocess.run([sys.executable, str(root/'tests/posix/posix_suite.py'),
                        '--worker', 'dir_deep_nesting', str(work/'worker-case'), str(result_path)],
                        env=environment, timeout=10, capture_output=True, text=True)
            assert result.returncode == 0 and result_path.read_text().startswith('PASS')
        worker_lines = (work/'posix-case-phases.log').read_text().splitlines()
        assert 'event=worker-entry ' in worker_lines[0] and 'event=case-complete ' in worker_lines[-1]
        # The retained observer limit applies even when the sender submits extra records.
        with Collector(work, True) as observer:
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sender:
                submitted = 0; deadline = time.monotonic() + 2
                while submitted < 140:
                    try:
                        sender.sendto(b'bounded-test\n', str(observer.path)); submitted += 1
                    except OSError:
                        assert time.monotonic() < deadline
                        time.sleep(0.001)
        bound = json.loads((work/'posix-case-phases-summary.json').read_text())
        assert bound['received'] == 140 and bound['retained'] == 128
        # Execute the actual fixture's shutdown guard with a failing stop.
        outer = next(n for n in tree.body if isinstance(n, ast.Try) and n.finalbody)
        guard = next(n for n in outer.finalbody if isinstance(n, ast.Try) and
                     any(isinstance(x, ast.Call) and isinstance(x.func, ast.Name) and x.func.id == 'stop'
                         for x in ast.walk(n)))
        observer = Collector(work, True); observer.__enter__()
        def failed_stop(): raise RuntimeError('controlled daemon stop')
        try:
            exec(compile(ast.Module(body=[guard], type_ignores=[]), '<actual-shutdown>', 'exec'),
                 dict(stop=failed_stop, query_work_observer=observer, handles=[]))
            raise AssertionError('stop exception suppressed')
        except RuntimeError as error:
            assert str(error) == 'controlled daemon stop'
        assert not observer.path.exists() and not observer.thread.is_alive()


        print('actual deep60 operations unchanged; default quiet; ordered native observer phases; missing/full sink nonblocking; failed leaf phase/status and observer cleanup PASS')
finally:
    os.environ.clear(); os.environ.update(saved)
