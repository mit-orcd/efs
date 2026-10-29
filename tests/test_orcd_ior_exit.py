#!/usr/bin/env python3
"""A failed MPI/IOR case must fail its runner while retaining every case result."""
import csv
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='efs-ior-exit-') as td:
    d = Path(td)
    (d/'bin').mkdir()
    (d/'mpivars.sh').write_text(':\n')
    (d/'bin/timeout').write_text('#!/bin/sh\nshift\nexec "$@"\n')
    (d/'bin/mpirun').write_text('''#!/usr/bin/env python3
import os, sys
if os.environ['MOCK_WIDE_FAIL'] == '1' and any(a.endswith('/wide') for a in sys.argv):
    print('synthetic MPI setup error', file=sys.stderr)
    sys.exit(255)
print('write 123.0')
print('read 456.0')
''')
    for f in (d/'bin').iterdir():
        f.chmod(0o700)
    for fail in (False, True):
        out = d/('failed' if fail else 'passed')
        mount = d/('mount-failed' if fail else 'mount-passed')
        mount.mkdir()
        env = dict(os.environ, PATH=str(d/'bin')+os.pathsep+os.environ['PATH'],
                   MPIVARS=str(d/'mpivars.sh'), IOR='synthetic-ior', HOSTS='localhost',
                   PPN='1', MOCK_WIDE_FAIL=str(int(fail)))
        run = subprocess.run(['bash', str(root/'scripts/orcd_test_ior.sh'), str(out), str(mount)],
                             env=env, text=True, capture_output=True, timeout=20)
        rows = list(csv.DictReader((out/'summary.tsv').open(), delimiter='\t'))
        assert len(rows) == 4, (run.stdout, run.stderr, rows)
        assert run.returncode == int(fail), (run.returncode, run.stdout, run.stderr)
        assert [int(r['rc']) for r in rows] == ([0, 0, 255, 255] if fail else [0]*4)
        assert all((out/(r['case']+'.log')).is_file() for r in rows)
print('ORCD IOR runner: all four results retained; MPI setup failures propagate; all-pass returns zero PASS')
