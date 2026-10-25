#!/usr/bin/env python3
"""scripts/d4_progress.py (alpha 1.0 D4 driver): its parsers on canned
efs-mgmt and `.efs/stats` output, and its verdict on synthetic samples with
one negative control per bound, so a wrong parse or a missed bound cannot
turn a cluster run into a false PASS."""
import argparse, importlib.util, json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('d4_progress', root / 'scripts/d4_progress.py')
d = importlib.util.module_from_spec(spec)
spec.loader.exec_module(d)

RAFT = '''node 1 root=1 salt=7 groups=2
  bound kv_mt_bytes=123456 kv_mem_budget=4194304 kv_l0_bytes=2097152 kv_l0_cap=1073741824 kv_apply_deferred=0 admit_busy=0 kv_flushes=4 kv_compactions=2
  leaders group0=node1 group2=node2
  group 0 hosted=1 role=leader leader=0 term=2 commit=900 applied=899 voters=0x7 joint=0 retired=0 last=900 last_term=2 snap=0 rejects_sent=0 proto_drops=0 unapplied=1 max_unapplied=40 busy_sent=0 admit_busy=0 low_capacity=0 campaigns_held=0 capacity_stepdowns=0
  group 2 hosted=1 role=follower leader=1 term=2 commit=700 applied=690 voters=0x7 joint=0 retired=0 last=700 last_term=2 snap=0 rejects_sent=0 proto_drops=0 unapplied=10 max_unapplied=12 busy_sent=0 admit_busy=0 low_capacity=0 campaigns_held=0 capacity_stepdowns=0
'''
b = d.parse_raft_bound(RAFT)
assert b == {'mt': 123456, 'budget': 4194304, 'l0': 2097152, 'l0_cap': 1073741824, 'flushes': 4,
             'compactions': 2, 'unapplied': 10}, b
assert d.parse_raft_bound('node 1 root=1\n') is None

PACK = '''node 1 127.0.0.1:1: leads group(s) 0, summary only, 0 segments; segments=3 live=10 KiB dead=0 KiB holed=0 underfilled=1 compacted_total=5
  backlog group=0 candidates=17 lag_ms=250 counted_ms_ago=10
  packer: passes=9 candidates=40 built=3 files_packed=30 cancelled=0 unindexed=0 backfilled=0 read_errors=0 put_errors=1 admit_errors=0 abandoned=0 busy_passes=0 budget_waits=0 abandon_scanned=0 abandon_actionable=0 abandon_refused=0 base_segments=0 base_files=0
node 2 127.0.0.1:2: leads group(s) 2, summary only, 0 segments; segments=1 live=1 KiB dead=0 KiB holed=0 underfilled=0 compacted_total=2
  backlog group=2 candidates=0 lag_ms=0 counted_ms_ago=3
  packer: passes=4 candidates=4 built=1 files_packed=4 cancelled=0 unindexed=0 backfilled=0 read_errors=2 put_errors=0 admit_errors=0 abandoned=0 busy_passes=0 budget_waits=0 abandon_scanned=0 abandon_actionable=0 abandon_refused=0 base_segments=0 base_files=0
node 3 127.0.0.1:3: leads no group
  packer: passes=0 candidates=0 built=0 files_packed=0 cancelled=0 unindexed=0 backfilled=0 read_errors=0 put_errors=0 admit_errors=0 abandoned=0 busy_passes=0 budget_waits=0 abandon_scanned=0 abandon_actionable=0 abandon_refused=0 base_segments=0 base_files=0
'''
p = d.parse_pack(PACK)
assert p['backlog'] == {0: 17, 2: 0} and p['lag_ms'] == {0: 250, 2: 0}, p
assert p['files_packed'] == 34 and p['compacted'] == 7 and p['put_errors'] == 1 and p['read_errors'] == 2, p
assert p['led'] == {0, 2}, p

STATS = json.dumps({'operation': 'stats', 'snapshot': {
    'boot_id': 1, 'generation': 12, 'pending_changes_at_least': 256, 'pending_age_ms_at_sample': 1500,
    'refresh_error': 0, 'refresh_overdue': False}})
ix = d.parse_index(STATS)
assert ix == {'generation': 12, 'pending': 256, 'pending_age_ms': 1500, 'pending_age_at_sample_ms': 1500,
              'error': 0, 'overdue': False}, ix
# The age now: the sample's age plus the time since the sample (a refresh
# that falls behind keeps an old sample); nothing pending is age 0.
LAG = json.loads(STATS)
LAG['snapshot']['lag_sample_ns'] = 10_000_000_000
assert d.parse_index(json.dumps(LAG), 12_500_000_000)['pending_age_ms'] == 4000
LAG['snapshot']['pending_changes_at_least'] = 0
assert d.parse_index(json.dumps(LAG), 12_500_000_000)['pending_age_ms'] == 0
assert d.rss_kib('Name:\tefs-fuse\nVmRSS:\t  20480 kB\n') == 20480
assert d.rss_kib('Name:\tx\n') is None


def args(**kw):
    a = dict(index_ready_s=60.0, fg_deadline_s=10.0, client_rss_mib=1024.0, server_rss_mib=2048.0, pack_backlog=1000,
             index_age_s=60.0, drain_s=100.0, no_l0_check=False, threads=2, load_s=3.0, working_set=0, fg_set=0,
             plateau_pct=None)
    a.update(kw)
    return argparse.Namespace(**a)


def sample(t, phase, comp=0, mt=5, packed=0, gen=0, backlog=5, age=100, crss=10240, srss=20480):
    node = {'mt': mt, 'budget': 100, 'l0': 10, 'l0_cap': 1000, 'flushes': comp * 2, 'compactions': comp,
            'unapplied': 1, 'rss_kib': srss}
    return {'t': t, 'phase': phase, 'client_rss_kib': {'/m': crss},
            'nodes': {1: dict(node), 2: dict(node)},
            'pack': {'backlog': {0: backlog, 2: 0}, 'lag_ms': {0: 1, 2: 0}, 'files_packed': packed,
                     'compacted': 0, 'put_errors': 0, 'read_errors': 0, 'led': [0, 2]},
            'index': {'generation': gen, 'pending': 1, 'pending_age_ms': age, 'error': 0, 'overdue': False}}


def run(a=None, edit=None, drained=5.0, bad=()):
    r = d.Run.__new__(d.Run)
    r.a = a or args()
    r.samples = [sample(0, 'before'), sample(1, 'load', comp=1, packed=1, gen=1),
                 sample(2, 'load', comp=2, packed=3, gen=2), sample(3, 'load-end', comp=3, packed=4, gen=3),
                 sample(4, 'drain', backlog=0)]
    r.ops = {'write+fsync 1 MiB': [0.5, 0.7], 'read 1 MiB': [0.1]}
    r.overdue, r.errors, r.stalls = [], [], 0
    r.counts = {'created': 100, 'unlinked': 10, 'create_errors': 0}
    r.live = {f'f{i}': 1 for i in range(90)}
    r.fg_files = {'g0': 1}
    r.members = {1: 'a:1', 2: 'b:2'}
    r.index_ready, r.index_not_ready = 0.5, [0.1]
    r.uncertain = set()
    r.lock = __import__('threading').Lock()
    r.t0 = __import__('time').time()
    if edit:
        edit(r)
    return r.verdict(r.samples[0], r.samples[3], drained, list(bad), 0.5)


v, fails, m = run()
assert v == 'PASS' and not fails, fails
assert m['kv_compactions_during_load'] == {1: 2, 2: 2} and m['files_packed_during_load'] == 3, m
assert m['kv_flushes_during_load'] == {1: 4, 2: 4}, m
assert m['index_generations_during_load'] == 2 and m['pack_backlog_max'] == 5, m
assert m['client_rss_mib_max'] == 10.0 and m['server_rss_mib_max'] == 20.0, m


def fails_with(text, **kw):
    v, fails, _ = run(**kw)
    assert v == 'FAIL' and any(text in f for f in fails), (text, fails)


def over(r):
    r.overdue = [('read 1 MiB', 11.2)]
fails_with('foreground operations over', edit=over)


def err(r):
    r.errors = ['read: differs']
fails_with('operation errors', edit=err)


def crss(r):
    r.samples[2]['client_rss_kib']['/m'] = 2 * 1024 * 1024
fails_with('client RSS', edit=crss)


def srss(r):
    r.samples[2]['nodes'][2]['rss_kib'] = 3 * 1024 * 1024
fails_with('server RSS', edit=srss)


def mt(r):
    r.samples[2]['nodes'][1]['mt'] = 101
fails_with('kv_mt_bytes over kv_mem_budget', edit=mt)


def l0(r):
    r.samples[1]['nodes'][1]['l0'] = 1001
fails_with('kv_l0_bytes over kv_l0_cap', edit=l0)


def backlog(r):
    r.samples[2]['pack']['backlog'][0] = 1001
fails_with('pack backlog', edit=backlog)


def age(r):
    r.samples[2]['index']['pending_age_ms'] = 61000
fails_with('index pending age', edit=age)


def nopack(r):
    for s in r.samples:
        s['pack']['files_packed'] = 7
fails_with('packed nothing', edit=nopack)


def noindex(r):
    for s in r.samples:
        s['index']['generation'] = 3
fails_with('never refreshed', edit=noindex)


def nocomp(r):
    for s in r.samples:
        for n in s['nodes'].values():
            n['compactions'] = 3
fails_with('no KV compaction', edit=nocomp)
v, fails, _ = run(a=args(no_l0_check=True), edit=nocomp)
assert v == 'PASS', fails


def noflush(r):
    for s in r.samples:
        for n in s['nodes'].values():
            n['flushes'] = 9
fails_with('no KV memtable flush', edit=noflush)
fails_with('not drained', drained=None)
fails_with('not exact', bad=['/m/x'])


def never_ready(r):
    r.index_ready = None
fails_with('never answered', edit=never_ready)


# "not indexed yet": tolerated within the initial window (another member's
# catalog may still be missing after one answered), an error past it.
def early_not_ready(r):
    r.not_ready(OSError(61, 'not indexed yet'))
    assert r.index_not_ready == [0.1, 0.0] and not r.errors, (r.index_not_ready, r.errors)
v, fails, m = run(edit=early_not_ready)
assert v == 'PASS' and m['index_not_ready_initial'] == [0.1, 0.0], (fails, m)


def late_not_ready(r):
    r.t0 -= 100
    r.not_ready(OSError(61, 'not indexed yet'))
fails_with('operation errors', edit=late_not_ready)


def unsampled(r):
    for s in r.samples:
        s['client_rss_kib'] = {'/m': None}
fails_with('client RSS', edit=unsampled)


# Fixed working set (tests/test_d4_plateau.py): created == live == threads x
# set, every operation kind ran; net creation or a missing kind fails.
def fixed(r):
    r.counts.update(created=20, overwritten=5, truncated=4, rewritten=3)
    r.live = {f'f{i}': 1 for i in range(20)}
v, fails, m = run(a=args(working_set=10), edit=fixed)
assert v == 'PASS' and m['working_set'] == {'files': 20, 'created': 20, 'live': 20, 'populate_s': None,
                                            'overwritten': 5, 'truncated': 4, 'rewritten': 3}, (fails, m)


def net_creation(r):
    fixed(r)
    r.counts['created'] = 21
    r.live['f20'] = 1
fails_with('net creation', a=args(working_set=10), edit=net_creation)


def no_truncate(r):
    fixed(r)
    r.counts['truncated'] = 0
fails_with('not overwritten, truncated and rewritten', a=args(working_set=10), edit=no_truncate)


# RSS plateau (scripts/rss_plateau.py over the driver's own samples): flat
# over the last third of the load passes, growth over 5 % of the peak fails,
# and samples after the load (drain, verify) do not count.
def flat(r):
    r.samples = [sample(t, 'load', comp=t + 1, packed=t + 1, gen=t + 1, crss=100000 + min(t, 4) * 10000,
                        srss=50000 + min(t, 4) * 1000) for t in range(12)]
    r.samples.append(sample(12.5, 'drain', comp=13, packed=13, gen=13, crss=300000, srss=90000))
v, fails, m = run(a=args(load_s=12.0, plateau_pct=5.0), edit=flat)
assert v == 'PASS', fails
assert [x['verdict'] for x in m['rss_plateau']] == ['PLATEAU'] * 3 and m['rss_plateau'][0]['samples'] == 12, m['rss_plateau']


def growing(r):
    r.samples = [sample(t, 'load', comp=t + 1, packed=t + 1, gen=t + 1, crss=100000 + t * 10000) for t in range(12)]
fails_with('not at a plateau', a=args(load_s=12.0, plateau_pct=5.0), edit=growing)
rows = d.rss_plateau.evaluate({'p': [(t, 1000 + t * 100) for t in range(9)]}, None, 5.0)
assert rows[0]['verdict'] == 'GROWING' and rows[0]['growth_pct'] > 5, rows
rows = d.rss_plateau.evaluate({'p': [(t, 1000 + min(t, 5) * 100) for t in range(9)], 'q': [(0, 1)]}, None, 5.0)
assert [x['verdict'] for x in rows] == ['PLATEAU', 'too few'], rows

print('D4 driver: raft-status, pack-compact and .efs/stats parsers; PASS and one negative control per '

      'bound (deadline, errors, client/server RSS, memtable, L0, pack backlog, index age, packing, index, '
      'flush and compaction progress, index readiness, '
      'drain, exact files, fixed working set, RSS plateau) PASS')
