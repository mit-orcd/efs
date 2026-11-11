#!/usr/bin/env python3
"""Mutation proof for tests/test_put_window_callers.py: delete each
put_win_close call in src/client/write.c in turn and list the scenarios that
fail. A close whose deletion fails no scenario is not covered.
Usage: python3 scripts/put_window_mutation.py   (prints a table)"""
import os, re, subprocess, sys, tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[1]
src = (root / 'src/client/write.c').read_text().split('\n')
sites = [i for i, l in enumerate(src) if re.search(r'\bput_win_close\(', l)]
scen = ['ok', 'fail', 'retired', 'drain', 'nomem', 'fetch', 'fetch_retired']
test = str(root / 'tests/test_put_window_callers.py')
print('line | failing scenarios')
with tempfile.TemporaryDirectory(prefix='efs-pw-mut-') as d:
    for i in sites:
        m = list(src)
        m[i] = re.sub(r'put_win_close\([^;]*\);', ';', m[i])
        f = Path(d) / 'write.c'
        f.write_text('\n'.join(m))
        env = dict(os.environ, WRITER_TEST_SOURCE=str(f))
        bad = [s for s in scen if subprocess.run([sys.executable, test, s], env=env,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode]
        print('write.c:%d | %s' % (i + 1, ', '.join(bad) or 'NONE (not covered)'))
