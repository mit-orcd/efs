#!/usr/bin/env python3
"""D4: the `.efs` index of a freshly formed cluster is published promptly,
and `.efs` queries never fail EIO while it is not.

Found by scripts/d4_progress.py through scripts/qualify.py on a new loopback
cluster: the seed starts alone, so its first catalog capture runs before the
cluster has formed and is refused EFS_ERR_UNFORMED. That refusal counted as
a build failure: the next attempt waited the whole refresh interval (60 s by
default), and for that minute every `.efs/stats` answered by the seed failed
EIO with the recorded error (104 of 104 queries in the D4 gate's first 60 s).
Now UNFORMED is an unsettled authority like BUSY and NOT_PRIMARY: the first
catalog is retried after about a second, and a query before it is the
ordinary "not indexed yet" (ENODATA) that callers wait on.

Three real efsd daemons on loopback (tests/k33_daemons.py), started 1 s
apart as scripts/qualify.py --loopback does, with the production refresh
interval (EFS_QUERY_REFRESH_MS=60000, so a missed first catalog shows),
private stores, free ports. The refusal is a race at formation (the commands
policy row lands just before the member counts itself formed) that a live
formation rarely shows, so two clusters are formed: the first with
EFS_FAULT_QUERY_FIRST_UNFORMED=1 on every member (tests/faults/efsd), which
refuses each member's first capture UNFORMED exactly as the race does
(`query-refresh: rc=-29 ... initial=1`), the second without the hook (the
natural formation; tests/test_query_refresh.c covers the scheduling
deterministically). On both:
1. every member publishes its first catalog within FIRST_BOUND_S of
   formation, the members refused UNFORMED too;
2. `.efs/stats --json` through a mount, repeated until every member has
   published: never EIO (answers, or "not indexed yet").
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import json, os, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, Mount, fuse_ok  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
FIRST_BOUND_S = 15.0
work = Path(tempfile.mkdtemp(prefix='efs-qfirst-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items()
       if not k.startswith('EFS_FAULT_') and not k.startswith('EFS_TEST_')}
env.update(EFS_QUERY_REFRESH_MS='60000')
t0 = time.time()
worst, refused = 0.0, {}
try:
    for k, forced in ((0, True), (1, False)):
        cenv = dict(env, EFS_FAULT_QUERY_FIRST_UNFORMED='1') if forced else env
        c = Cluster(work, f'q{k}', [1, 2, 3], cenv)
        mounts = []
        try:
            for i in (1, 2, 3):
                c.start(i, join=None if i == 1 else 1)
                time.sleep(1)
            for i in (1, 2, 3):
                c.wait_log(i, r'raft-host: formed: 3 members')
            formed = time.time()
            for i in (1, 2, 3):
                c.wait_log(i, r'query-publish: generation=1 ', timeout=FIRST_BOUND_S)
            worst = max(worst, time.time() - formed)
            hit = [i for i in (1, 2, 3) if re.search(r'query-refresh: rc=-29 .*initial=1', c.log(i))]
            fired = [i for i in (1, 2, 3)
                     if 'fault: EFS_FAULT_QUERY_FIRST_UNFORMED refused the first catalog capture' in c.log(i)]
            if forced:
                assert fired == [1, 2, 3], f'the hook did not fire on every member: {fired}'
                assert hit == [1, 2, 3], f'refused members {hit}: ' + ''.join(
                    c.log(i)[-1500:] for i in (1, 2, 3) if i not in hit)
            else:
                assert not fired, fired
            refused[k] = hit
            if fuse_ok():
                m = Mount(c, 1, 'mnt')
                mounts.append(m)
                end, answered, nodata = time.time() + 30, 0, 0
                while time.time() < end and answered < 5:
                    p = subprocess.run([sys.executable, str(m.path / '.efs' / 'stats'), '--json'],
                                       capture_output=True, text=True, timeout=30, cwd=m.path)
                    if p.returncode == 0:
                        json.loads(p.stdout.strip().splitlines()[-1])['snapshot']
                        answered += 1
                    else:
                        assert 'not indexed yet' in p.stderr, \
                            f'.efs/stats failed: {p.stderr.strip()[-400:]}'
                        nodata += 1
                    time.sleep(0.2)
                assert answered >= 5, f'.efs/stats answered {answered} times ({nodata} not indexed yet)'
        finally:
            for mt in mounts:
                mt.close()
            c.stop_all()
    natural = f'member(s) {refused[1]}' if refused[1] else 'no member'
    print(f'1-2. forced: every member refused UNFORMED at formation; natural formation: {natural} '
          f'refused; every first catalog within {worst:.1f} s of formation (bound {FIRST_BOUND_S} s), '
          '.efs/stats never EIO PASS')
finally:
    if os.environ.get('EFS_QFIRST_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print(f'D4 first .efs catalog after formation: prompt, queries never EIO: PASS ({time.time() - t0:.1f} s)')
