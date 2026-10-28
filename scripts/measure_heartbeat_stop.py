#!/usr/bin/env python3
"""Measure checked idle-daemon stops; keep logs/results outside the source tree.

No mount, real cluster or reserved endpoint is used. Stores are retained under
--work for explicit cleanup after the closed logs and results are verified.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, required=True)
p.add_argument('--work', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--rounds', type=int, default=3)
a = p.parse_args()
assert a.rounds > 0
sys.path.insert(0, str(a.source.resolve() / 'tests'))
import k33_daemons as k
k.EFSD = a.source.resolve() / 'efsd'
a.work.mkdir(parents=True, exist_ok=False)
a.output.mkdir(parents=True, exist_ok=False)
env = dict(os.environ, EFS_TRANSPORT='tcp')
version = subprocess.check_output([str(k.EFSD), '--version'], text=True).strip()
results = {'version': version, 'transport': 'tcp', 'formed': False, 'samples': []}
for n in range(a.rounds):
    for offset in (0.05, 0.3, 0.8):
        label = 'run-%d-offset-%.2f' % (n, offset)
        c = k.Cluster(a.work, label, [1], env)
        try:
            proc = c.start(1)
            c.wait_log(1, r'node 1 listening on ')
            time.sleep(offset)
            started = time.monotonic()
            proc.send_signal(signal.SIGTERM)
            rc = proc.wait(timeout=60)
            elapsed = time.monotonic() - started
            record = c.dir / 'n1/log/efsd.exit'
            text = record.read_text()
            assert rc == 0 and 'outcome=orderly' in text and 'rc=0' in text, (rc, text)
            for barrier in ('put_sync', 'reclaim_journal', 'usage_flush'):
                assert barrier + '=done' in text, text
            shutil.copyfile(c.dir / 'n1.log', a.output / (label + '.log'))
            shutil.copyfile(record, a.output / (label + '.exit'))
            results['samples'].append({'case': label, 'offset_s': offset, 'stop_s': elapsed,
                                       'rc': rc, 'exit_record': text})
            (a.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(label, 'orderly_stop_s=%.4f' % elapsed, flush=True)
        finally:
            c.stop_all()
print('PASS:', len(results['samples']), 'checked orderly stops; no forced discard or mounts')
