#!/usr/bin/env python3
"""D4 with a fixed working set and the F1 RSS plateau (alpha 1.0 checklist
D4 and F1), loopback version.

The same cluster as tests/test_d4_progress.py (three efsd of the EFS_FAULTS
build and two efs-fuse on loopback, private stores under TMPDIR, free ports,
GC on; packer 1 s minimum age and 200 ms passes, `.efs` index refresh every
2 s, 256 KiB memtables so the KV flushes and compacts), but the load has no
net creation: every writer creates EFS_D4_SET files once (every eighth
192–512 KiB, two to four chunks, never packed; the rest 1 B–8 KiB, packed)
and then overwrites (O_TRUNC), truncates and rewrites, or rewrites in place
a random one of them with a new version, while the foreground rewrites a
ring of EFS_D4_FG_SET 1 MiB files and reads them back
(scripts/d4_progress.py --working-set, --fg-set). So the packer unindexes
and repacks, segments get holes and compaction, old generations go to
reclaim, the index refreshes and the KV flushes and compacts, with the
namespace fixed: what grows then is memory the daemons keep per unit of
work, not per file.

Checks: every D4 bound of the driver, no net creation (created == live ==
threads × set), every operation kind ran, KV compactions on every member,
no packer put or read error, every file exact at its last version. The RSS
of every efs-fuse and efsd is sampled once a second; the plateau table
(scripts/rss_plateau.py: growth over the last third of the load as a share
of the peak) is printed always and enforced at 5 % when the load is at least
EFS_D4_PLATEAU_MIN_S (default 600) seconds long or EFS_D4_PLATEAU=1: a
gate-length run is still warming up. The F1 measurement is
EFS_D4_LOAD_S=1800 EFS_D4_QUOTA=8G. EFS_D4_ARTIFACTS=DIR keeps the driver's
JSON there; EFS_D4_THREADS, EFS_D4_REFRESH_MS and EFS_D4_KEEP as in
test_d4_progress.py.
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
    print('D4 plateau: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
LOAD_S = float(os.environ.get('EFS_D4_LOAD_S', '90'))
THREADS = int(os.environ.get('EFS_D4_THREADS', '8'))
WORKING_SET = int(os.environ.get('EFS_D4_SET', '256'))
FG_SET = int(os.environ.get('EFS_D4_FG_SET', '32'))
PLATEAU_MIN_S = float(os.environ.get('EFS_D4_PLATEAU_MIN_S', '600'))
PLATEAU = bool(os.environ.get('EFS_D4_PLATEAU')) or LOAD_S >= PLATEAU_MIN_S
PLATEAU_PCT = 5.0
FG_DEADLINE_S = 10.0
DRAIN_S = 150.0
# The D4 bounds; a sanitizer build (make asan-check / tsan-check) carries
# shadow memory and redzones, so its bounds scale as in test_d4_progress.py.
RSS_SCALE = 6 if cflags() else 1
CLIENT_RSS_MIB = 1024 * RSS_SCALE
SERVER_RSS_MIB = 2048 * RSS_SCALE
PACK_BACKLOG = 50000
INDEX_AGE_S = 60.0
# Per-store quota: the population is small (8 × 256 files, about 90 MiB of
# large files) but every rewrite leaves a generation for reclaim; a
# 30-minute run passes EFS_D4_QUOTA=8G.
QUOTA = os.environ.get('EFS_D4_QUOTA', '2G')
work = Path(tempfile.mkdtemp(prefix='efs-d4p-', dir=os.environ.get('TMPDIR')))
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
    argv = ['--mount', str(load.path), '--fg-mount', str(fg.path), '--seed', c.ep(1), '--mgmt', str(MGMT),
            '--out', str(out), '--load-s', str(LOAD_S), '--threads', str(THREADS),
            '--working-set', str(WORKING_SET), '--fg-set', str(FG_SET),
            '--fg-deadline-s', str(FG_DEADLINE_S), '--drain-s', str(DRAIN_S), '--stall-s', '30',
            '--client-rss-mib', str(CLIENT_RSS_MIB), '--server-rss-mib', str(SERVER_RSS_MIB),
            '--pack-backlog', str(PACK_BACKLOG), '--index-age-s', str(INDEX_AGE_S)]
    if PLATEAU:
        argv += ['--plateau-pct', str(PLATEAU_PCT)]
    rc = d4_progress.main(argv)
    doc = json.loads((out / 'd4-result.json').read_text())
    m = doc['measured']
    samples = json.loads((out / 'd4-samples.json').read_text())
    rows = m.get('rss_plateau') or d4_progress.rss_plateau.evaluate(
        d4_progress.rss_plateau.from_samples(samples), LOAD_S, PLATEAU_PCT)
    print(f'RSS over the {LOAD_S:.0f} s load ({"enforced" if PLATEAU else "reported"}, criterion '
          f'{PLATEAU_PCT:.0f} % of the peak over the last third):')
    print(d4_progress.rss_plateau.format_rows(rows))
    assert rc == 0, f'D4 driver: {doc["verdict"]}: {doc["reason"]}\n' + json.dumps(m, indent=1)[:4000]
    ws = m['working_set']
    assert ws['created'] == ws['live'] == THREADS * WORKING_SET, ws
    for i in (1, 2, 3):
        assert 'kv-fault: memtable 262144 bytes' in c.log(i), f'node {i}: the memtable hook did not apply'
    comp = {i: compactions(c, i, started) for i in (1, 2, 3)}
    assert all(n > 0 for n in comp.values()), f'KV compactions per member during the run: {comp}'
    last = next(s['pack'] for s in reversed(samples) if s.get('pack'))
    assert last['put_errors'] == 0 and last['read_errors'] == 0, f'packer errors: {last}'
    m['kv_compactions_logged'] = comp
    (out / 'd4-result.json').write_text(json.dumps(doc, indent=1, default=str))
    lat = ', '.join(f'{k} max {v["max_s"]} s' for k, v in sorted(m['fg_latency'].items()))
    print(f'D4 plateau: working set {ws["files"]} files ({ws["overwritten"]} overwritten, {ws["truncated"]} '
          f'truncated, {ws["rewritten"]} rewritten, no net creation); {m["fg_ops"]} foreground ops within '
          f'{FG_DEADLINE_S} s ({lat}); client RSS {m["client_rss_mib_max"]} MiB <= {CLIENT_RSS_MIB}, server RSS '
          f'{m["server_rss_mib_max"]} MiB <= {SERVER_RSS_MIB}; kv_mt max {m["kv_mt_max"]} <= budget '
          f'{m["kv_mem_budget"]}, L0 max {m["kv_l0_max"]} <= cap {m["kv_l0_cap"]}; pack backlog max '
          f'{m["pack_backlog_max"]} <= {PACK_BACKLOG}; index pending age max {m["index_pending_age_ms_max"]} ms '
          f'<= {INDEX_AGE_S * 1000:.0f}; packed {m["files_packed_during_load"]} files, '
          f'{m["index_generations_during_load"]} index generations, KV flushes {m["kv_flushes_during_load"]} '
          f'and compactions {m["kv_compactions_during_load"]} during the load; drained in {m["drain_s"]} s '
          f'<= {DRAIN_S}; {m["load_live_files"] + m["fg_files"]} files exact')
finally:
    for mt in mounts:
        mt.close()
    c.stop_all()
    if os.environ.get('EFS_D4_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print(f'D4 fixed working set under packing, indexing and KV compaction'
      f'{", RSS plateau" if PLATEAU else ""}: PASS ({time.time() - t0:.1f} s)')
