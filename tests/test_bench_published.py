#!/usr/bin/env python3
"""W170: published benchmark writes must survive in an independent FUSE reader.

Three private TCP/O_DIRECT members, 1 GiB quota each. Checked publication,
exact byte/EOF/count checks, wrong-length/corrupt-payload refusal, strict stop.
No application retry and no acceptance of unpublished or failed writes.
"""
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import k33_daemons as k

ROOT = Path(__file__).resolve().parents[1]
if not k.fuse_ok():
    print('NOTRUN: published benchmark requires FUSE prerequisites')
    raise SystemExit(0)
k.EFSD = ROOT / 'efsd'  # Normal production daemon, no test hooks.
env = {key: value for key, value in os.environ.items() if not key.startswith('EFS_')}
env['EFS_TRANSPORT'] = 'tcp'
work = Path(tempfile.mkdtemp(prefix='efs-w170-', dir=os.environ.get('TMPDIR')))
c = k.Cluster(work, 'bench', [1, 2, 3], env)
m = None

def bench(*args, ok=True):
    cmd = [str(ROOT/'efs-bench'), c.ep(1), *map(str, args)]
    r = subprocess.run(cmd, env=env, text=True, capture_output=True, timeout=90)
    print('$', ' '.join(cmd), '\n', r.stdout, r.stderr, flush=True)
    assert (r.returncode == 0) == ok, (r.returncode, r.stdout, r.stderr)
    return r.stdout

try:
    c.start(1, quota='1G')
    c.start(2, join=1, quota='1G')
    c.start(3, join=1, quota='1G')
    c.wait_members((1, 2, 3), 3)
    c.wait_voters((1, 2, 3), lambda n, v: k.voters_ok(n, v, 3))
    total, writers = 2*1024*1024+67, 3
    out = bench('--size', total, '--io-size', '64K', '--workers', writers, '--id', 170)
    assert f'BENCH_OK kind=write logical_bytes={total} ' in out, out
    # A failed name collision cannot overwrite a previous benchmark fixture.
    bench('--size', total, '--workers', writers, '--id', 170, ok=False)
    bench('--meta', '--export', 'default', '--files', 5, '--dirs', 2,
          '--size', 4099, '--workers', 2, '--phases', 'mkdir,create', '--keep')
    # Start the reader only after the benchmark processes (and their caches) exit.
    m = k.Mount(c, 3, 'cold-reader')
    parents = sorted(m.path.glob('bench-data-170-w*'))
    assert [p.name for p in parents] == [f'bench-data-170-w{i}' for i in range(writers)]
    assert all([p.name for p in d.iterdir()] == ['payload'] for d in parents)
    for i in range(writers):
        want = total//writers + (i < total%writers)
        data = (m.path/f'bench-data-170-w{i}/payload').read_bytes()
        assert len(data) == want and data == b'A'*want, (i, len(data), want)
    meta = sorted(m.path.glob('bench-meta-*-d*'))
    assert len(meta) == 2, meta
    files = [p for d in meta for p in d.iterdir()]
    assert len(files) == 5
    assert all(p.read_bytes() == b'A'*4099 for p in files)
    out = bench('--read', '--size', total, '--io-size', '16M', '--workers', writers, '--id', 170)
    assert f'BENCH_OK kind=read logical_bytes={total} ' in out, out
    out = bench('--read', '--time', '.05', '--workers', writers, '--id', 170)
    assert 'BENCH_OK kind=read' in out, out
    bench('--read', '--size', total+3, '--workers', writers, '--id', 170, ok=False)
    target = m.path/'bench-data-170-w0/payload'
    with target.open('r+b', buffering=0) as f:
        f.write(b'B')
        os.fsync(f.fileno())
    out = bench('--read', '--size', total, '--workers', writers, '--id', 170, ok=False)
    assert 'BENCH_FAIL kind=read' in out, out
    print('W170 published benchmark: exact cold FUSE bytes/EOF/count, metadata payloads, same-API reads and negative controls PASS', flush=True)
finally:
    print('W170 fixture:', work, flush=True)
    if m is not None:
        stop = subprocess.run([str(ROOT/'scripts/client.sh'), 'stop', str(m.path)],
                              env=env, capture_output=True, text=True, timeout=180)
        print(stop.stdout, stop.stderr, flush=True)
        assert stop.returncode == 0 and not os.path.ismount(m.path), 'reader stop failed; stores preserved'
        assert m.p.wait(timeout=30) == 0
        m.log.close()
    for i, proc in c.procs.items():
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
        assert proc.wait(timeout=60) == 0, (i, 'daemon stop failed')
        record = c.dir/f'n{i}/log/efsd.exit'
        assert 'outcome=orderly' in record.read_text(), record.read_text()
    c.stop_all()
    for log in c.dir.glob('*.log'):
        print(f'==== {log.name} ====\n{log.read_text(errors="replace")}', flush=True)
