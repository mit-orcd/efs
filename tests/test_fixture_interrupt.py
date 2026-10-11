#!/usr/bin/env python3
"""Actual private fixture signal setup permits cleanup after background exec."""
import ast, os, signal, subprocess, sys, tempfile, time
from pathlib import Path
root = Path(__file__).resolve().parents[1]
source = Path(os.environ.get('EFS_FIXTURE_SOURCE', root / 'tests/live/gc_reclamation.py')).read_text()
tree = ast.parse(source)
setup = []
for node in tree.body:
    if isinstance(node, ast.FunctionDef) and node.name == 'interrupt_fixture':
        setup.append(ast.get_source_segment(source, node))
    elif isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
        call = node.value
        if isinstance(call.func, ast.Attribute) and isinstance(call.func.value, ast.Name) and call.func.value.id == 'signal' and call.func.attr == 'signal':
            setup.append(ast.get_source_segment(source, node))
with tempfile.TemporaryDirectory(prefix='efs-fixture-interrupt-') as directory:
    for sig in (signal.SIGINT, signal.SIGTERM):
        ready = Path(directory) / ('ready-' + str(sig))
        cleaned = Path(directory) / ('cleaned-' + str(sig))
        # The ignored disposition models nohup/background shell exec; use the
        # fixture's real setup, not a duplicate implementation of its handlers.
        script = 'import signal,time\nfrom pathlib import Path\nsignal.signal(signal.SIGINT,signal.SIG_IGN)\n' + '\n'.join(setup)
        script += '\ntry:\n Path(' + repr(str(ready)) + ').touch()\n while True: time.sleep(.05)\nfinally:\n Path(' + repr(str(cleaned)) + ').touch()\n'
        proc = subprocess.Popen([sys.executable, '-c', script], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 5
            while not ready.exists():
                assert proc.poll() is None and time.monotonic() < deadline, 'fixture did not become ready'
                time.sleep(.01)
            proc.send_signal(sig)
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                raise AssertionError('background fixture ignored shutdown signal') from None
            assert cleaned.exists(), 'fixture skipped Python finally cleanup'
        finally:
            if proc.poll() is None:
                proc.kill(); proc.wait()
print('background fixture SIGINT/SIGTERM finally cleanup PASS')
