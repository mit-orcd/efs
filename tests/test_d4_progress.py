#!/usr/bin/env python3
"""D4 (alpha 1.0 checklist): bounded progress with packing, `.efs` indexing
and KV maintenance all active, loopback version.

Three real efsd daemons on loopback (tests/k33_daemons.py, the EFS_FAULTS
build), private stores under TMPDIR, free ports (never 17xxx), GC on.
Background work is made busy on a short run, never switched off:
- the packer: 1 s minimum age, 200 ms passes;
- the `.efs` index: refresh every 2 s (EFS_QUERY_REFRESH_MS, at least 256
  changes also start one);
- KV maintenance: 256 KiB memtables (EFS_FAULT_MEMTABLE_BYTES), so the
  small-file load flushes to L0 and compacts many times a minute.
Mount L (node 1) carries the small-file load, mount F (node 2) the
foreground operations. scripts/d4_progress.py, the driver the parent session
runs on real hosts through scripts/qualify.py, runs the load for LOAD_S with
THREADS writers and checks every bound (its docstring): each foreground
operation within FG_DEADLINE_S, client and server RSS, kv memtable and L0
against their design bounds, pack backlog and index pending age, progress
during the load, drain within DRAIN_S after it, every acknowledged file
exact. On top, from the daemon logs: KV compactions completed during the
load (`kv-compact: end ... rc=0`) on every member, and no packer read or
put error. EFS_D4_ARTIFACTS=DIR keeps the driver's JSON there; EFS_D4_LOAD_S,
EFS_D4_THREADS and EFS_D4_REFRESH_MS change the load and the refresh;
EFS_D4_QUOTA the per-store quota (default 1G).
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import calendar, json, os, re, shutil, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, Mount, fuse_ok  # noqa: E402
from sanitize_env import cflags  # noqa: E402

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'scripts'))
import d4_progress  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('D4 bounded progress: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
LOAD_S = float(os.environ.get('EFS_D4_LOAD_S', '45'))
THREADS = int(os.environ.get('EFS_D4_THREADS', '8'))
# Per-store quota: 1G fits the 45 s default load; a 20-minute run (F1 RSS
# plateau) fills it (913 MB stores, ENOSPC), so long runs pass EFS_D4_QUOTA.
QUOTA = os.environ.get('EFS_D4_QUOTA', '1G')
FG_DEADLINE_S = 10.0
DRAIN_S = 150.0
# Under the F1 sanitizer gates (EFS_TEST_SANITIZE / EFS_TEST_TSAN) the
# daemons carry shadow memory, redzones and the ASan quarantine: the 60 s
# ASan load reached a 1264 MiB efs-fuse where the native run stays near
# 260 MiB, so the RSS bounds scale there; the native bound is the gate.
RSS_SCALE = 6 if cflags() else 1
CLIENT_RSS_MIB = 1024 * RSS_SCALE
SERVER_RSS_MIB = 2048 * RSS_SCALE
PACK_BACKLOG = 50000
INDEX_AGE_S = 60.0
work = Path(tempfile.mkdtemp(prefix='efs-d4-', dir=os.environ.get('TMPDIR')))
out = Path(os.environ['EFS_D4_ARTIFACTS']) if os.environ.get('EFS_D4_ARTIFACTS') else work / 'out'
env = {k: v for k, v in os.environ.items()
       if not k.startswith('EFS_FAULT_') and not k.startswith('EFS_TEST_')}
REFRESH_MS = os.environ.get('EFS_D4_REFRESH_MS', '2000')
env.update(EFS_PACK_MIN_AGE_MS='1000', EFS_PACK_INTERVAL_MS='200', EFS_QUERY_REFRESH_MS=REFRESH_MS,
           EFS_FAULT_MEMTABLE_BYTES=str(256 * 1024))
env.pop('EFS_GC_DISABLE', None)
t0 = time.time()


def compactions(c, i, since):
    """kv-compact end lines (rc=0) logged by node i after `since` (epoch s)."""
    n = 0
    for line in c.log(i).splitlines():
        m = re.match(r'(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d+)Z kv-compact: end .* rc=0', line)
        if m and calendar.timegm(time.strptime(m.group(1)[:19], '%Y-%m-%dT%H:%M:%S')) >= int(since):
            n += 1
    return n


c = Cluster(work, 'd', [1, 2, 3], env)
mounts = []
try:
    c.start(1, quota=QUOTA)
    c.start(2, join=1, quota=QUOTA)
    c.start(3, join=1, quota=QUOTA)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    c.wait_members([1, 2, 3], 3)
    load = Mount(c, 1, 'mnt-l')
    mounts.append(load)
    fg = Mount(c, 2, 'mnt-f')
    mounts.append(fg)
    started = time.time()
    rc = d4_progress.main([
        '--mount', str(load.path), '--fg-mount', str(fg.path), '--seed', c.ep(1), '--mgmt', str(MGMT),
        '--out', str(out), '--load-s', str(LOAD_S), '--threads', str(THREADS),
        '--fg-deadline-s', str(FG_DEADLINE_S), '--drain-s', str(DRAIN_S), '--stall-s', '30',
        '--client-rss-mib', str(CLIENT_RSS_MIB), '--server-rss-mib', str(SERVER_RSS_MIB),
        '--pack-backlog', str(PACK_BACKLOG), '--index-age-s', str(INDEX_AGE_S)])
    doc = json.loads((out / 'd4-result.json').read_text())
    m = doc['measured']
    assert rc == 0, f'D4 driver: {doc["verdict"]}: {doc["reason"]}\n' + json.dumps(m, indent=1)[:4000]
    for i in (1, 2, 3):
        assert 'kv-fault: memtable 262144 bytes' in c.log(i), f'node {i}: the memtable hook did not apply'
    comp = {i: compactions(c, i, started) for i in (1, 2, 3)}
    assert all(n > 0 for n in comp.values()), f'KV compactions per member during the run: {comp}'
    samples = json.loads((out / 'd4-samples.json').read_text())
    last = next(s['pack'] for s in reversed(samples) if s.get('pack'))
    assert last['put_errors'] == 0 and last['read_errors'] == 0, f'packer errors: {last}'
    m['kv_compactions_logged'] = comp
    (out / 'd4-result.json').write_text(json.dumps(doc, indent=1, default=str))
    lat = ', '.join(f'{k} max {v["max_s"]} s' for k, v in sorted(m['fg_latency'].items()))
    print(f'D4: {m["fg_ops"]} foreground ops within {FG_DEADLINE_S} s ({lat}); client RSS '
          f'{m["client_rss_mib_max"]} MiB <= {CLIENT_RSS_MIB}, server RSS {m["server_rss_mib_max"]} MiB '
          f'<= {SERVER_RSS_MIB}; kv_mt max {m["kv_mt_max"]} <= budget {m["kv_mem_budget"]}, '
          f'L0 max {m["kv_l0_max"]} <= cap {m["kv_l0_cap"]}; pack backlog max {m["pack_backlog_max"]} '
          f'<= {PACK_BACKLOG}; index pending age max {m["index_pending_age_ms_max"]} ms <= '
          f'{INDEX_AGE_S * 1000:.0f}; packed {m["files_packed_during_load"]} files, '
          f'{m["index_generations_during_load"]} index generations, KV flushes '
          f'{m["kv_flushes_during_load"]} and compactions {m["kv_compactions_during_load"]} during the load; drained in {m["drain_s"]} s '
          f'<= {DRAIN_S}; {m["load_live_files"] + m["fg_files"]} files exact')
finally:
    for mt in mounts:
        mt.close()
    c.stop_all()
    if os.environ.get('EFS_D4_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print(f'D4 bounded progress under packing, indexing and KV compaction: PASS ({time.time() - t0:.1f} s)')
