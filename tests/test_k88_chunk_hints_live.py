#!/usr/bin/env python3
"""K88 (Task DK) on real daemons: every chunk of a file keeps its own CAS hint.

The slot-less library writer (client_write_run, as efs-bench) records every
full chunk image at base 0; after a REPORT committed an earlier image of the
chunk, only the K88 hint (cbase) gives the next image the committed base.
tests/test_k88_chunk_hints (one process per file, three private production
efsd, TCP): write the first half of chunks 0 and 1, one sync REPORT commits
both half images; write both second halves (two new full images at base 0)
and REPORT again. Per-chunk hints: both publish, no STALE round. One hint per
inode (cbase_slot before Task DK dropped the chunk index): chunk 0 lost its
hint to chunk 1's note, goes STALE twice and `report-stuck` drops it as a
writeback error.

Pass: every driver exits 0 (both REPORTs OK, no writeback error, the file
reads back exact) and no `report-stuck` line. One summary line per file.
"""
import os, re, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import k33_daemons as k

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / 'tests/test_k88_chunk_hints'
FILES = int(os.environ.get('K88_CHUNK_FILES', '3'))
k.EFSD = ROOT / 'efsd'   # production daemon: the client path is under test
env = {key: value for key, value in os.environ.items() if not key.startswith('EFS_')}
env['EFS_TRANSPORT'] = 'tcp'
work = Path(tempfile.mkdtemp(prefix='efs-k88c-', dir=os.environ.get('TMPDIR')))
c = k.Cluster(work, 'k88c', [1, 2, 3], env)
t0 = time.time()
try:
    c.start(1); c.start(2, join=1); c.start(3, join=1)
    c.wait_members((1, 2, 3), 3)
    c.wait_voters((1, 2, 3), lambda n, v: k.voters_ok(n, v, 3))
    fails, stuck_total = [], 0
    for f in range(FILES):
        r = subprocess.run([str(DRIVER), c.ep(1), f'k88c-{f}'], env=env, text=True,
                           capture_output=True, timeout=60 * k.SLOW)
        stuck = len(re.findall(r'^report-stuck ', r.stderr, re.M))
        stuck_total += stuck
        line = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else 'no driver line'
        print(f'K88 chunk hints file={f} rc={r.returncode} report_stuck={stuck} {line}', flush=True)
        if r.returncode or stuck:
            fails.append(f'file {f}: rc={r.returncode} report-stuck={stuck}\n{r.stderr[-3000:]}')
    assert not fails, '\n'.join(fails)
    print(f'K88 per-chunk CAS hints on real daemons: {FILES} files, two chunks each, '
          f'report-stuck {stuck_total}: PASS ({time.time() - t0:.1f} s)')
finally:
    c.stop_all()
    if os.environ.get('K88_CHUNK_KEEP') or sys.exc_info()[0] is not None:
        print('kept', work)
    else:
        import shutil
        shutil.rmtree(work, ignore_errors=True)
