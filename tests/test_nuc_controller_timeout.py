#!/usr/bin/env python3
"""Actual loaded controller requests fixture cleanup before recording timeout."""
import ast, os, subprocess, sys, tempfile, time
from pathlib import Path
root = Path(__file__).resolve().parents[1]
source = (root / 'tests/measure/nuc_loaded_append_20261011.py').read_text()
node = next(n for n in ast.parse(source).body if isinstance(n, ast.FunctionDef) and n.name == 'run')
with tempfile.TemporaryDirectory(prefix='efs-controller-timeout-') as directory:
    out = Path(directory)
    namespace = dict(subprocess=subprocess,time=time,out=out,src=root,env=dict(os.environ))
    exec(ast.get_source_segment(source,node),namespace)
    cleaned = out / 'cleaned'
    script = 'import signal,time,sys\nfrom pathlib import Path\nsignal.signal(signal.SIGTERM,lambda *args:sys.exit(17))\ntry:\n while True:time.sleep(.02)\nfinally:\n Path(' + repr(str(cleaned)) + ').touch()\n'
    rc = namespace['run']('timeout-cleanup',[sys.executable,'-c',script],.2)
    assert rc == 17 and cleaned.exists(), 'timeout bypassed Python cleanup'
    assert (out / 'timeout-cleanup-timeout.txt').exists()
    assert (out / 'timeout-cleanup-exit.txt').read_text().strip() == '17'
print('loaded controller timeout requests cleanup and preserves nonzero verdict PASS')
