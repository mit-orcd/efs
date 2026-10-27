#!/usr/bin/env python3
"""W166: two independent close failure paths, with the 30 s budget unchanged.

Private three-voter TCP/O_DIRECT cluster. REPORT: park node 1's compactor,
fill its actual L0 to the compressed 1 MiB cap with metadata, then stop the
load while the bounded memtable tail still has admission room. A full L0
alone must not reject REPORT until its 8+8 second budgets expire. PUT:
move both leaders away from node 1, then force only its local ticket STATUS
read to lack an applied proof. It must delegate the complete authority read,
not keep returning NOT_PRIMARY for the same fragment until 30 seconds.
All successful payloads are verified through a fresh mount after faults clear.
Failures remain failures. The pre-fix instrumentation commit ddff5fcfd runs
both cases independently and fails; it changes no production behavior.
Temporary stores/logs are retained for the runner's reviewed cleanup.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, Mount, fuse_ok, mgmt, payload, root, place_leaders

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--case', choices=('both', 'report', 'put'), default='both')
a = parser.parse_args()
if not fuse_ok():
    print('W166 authority: NOTRUN (FUSE unavailable)')
    sys.exit(0)
work = Path(tempfile.mkdtemp(prefix='efs-w166-', dir=os.environ.get('TMPDIR')))
env = dict(os.environ, EFS_TRANSPORT='tcp', EFS_RPC_PROF='1', EFS_PUT_TICKET_DBG='1')
c = Cluster(work, 'authority', [1, 2, 3], env)
park, lag = work / 'compact-parked', work / 'ticket-read-lag'
mounts, confirmed, results, failures = [], {}, {}, []
t0 = time.monotonic()


def status():
    rc, out = mgmt(env, 'raft-status', c.ep(1))
    assert rc == 0, out
    with (work / 'status.log').open('a') as f:
        f.write(f'elapsed={time.monotonic()-t0:.3f}\n{out}\n')
    return out


def field(text, name):
    return int(re.search(r'\b' + name + r'=(\d+)', text).group(1))


def write_case(case, path):
    data = payload('w166-' + case, 2048)
    start = time.monotonic()
    try:
        fd = os.open(path, os.O_WRONLY)
        try:
            assert os.write(fd, data) == len(data)
        finally:
            os.close(fd)  # do not re-close or promote an ambiguous failure
        elapsed = time.monotonic() - start
        assert elapsed < 30, f'{case} exceeded unchanged 30 s deadline: {elapsed}'
        confirmed[path.name] = data
        results[case] = dict(outcome='PASS', seconds=elapsed)
    except (OSError, AssertionError) as e:
        results[case] = dict(outcome='FAIL', seconds=time.monotonic()-start, error=str(e))
        failures.append(f'{case}: {e}')
    print(case, results[case], flush=True)


try:
    c.start(1, extra_env={'EFS_FAULT_L0_BYTES': str(1 << 20),
                        'EFS_FAULT_MEMTABLE_BYTES': str(1 << 20),
                        'EFS_FAULT_COMPACT_STALL': '1',
                        'EFS_FAULT_COMPACT_FILE': str(park),
                        'EFS_FAULT_TICKET_READ_LAG_FILE': str(lag)})
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    place_leaders(c, 1)
    m = Mount(c, 2, 'writer')
    mounts.append(m)
    # Do not let inode allocation/open failures masquerade as a close failure.
    for name in ('report', 'put'):
        (m.path / name).write_bytes(b'')
    if a.case in ('both', 'report'):
        assert c.leader_placement(2) == {0: 1, 2: 1}
        park.touch()
        end, iteration = time.monotonic() + 90, 0
        while True:
            st = status()
            if field(st, 'kv_l0_bytes') >= field(st, 'kv_l0_cap'):
                break
            assert time.monotonic() < end, 'did not fill L0'
            iteration += 1
            p = subprocess.run([str(root / 'efs-bench'), c.ep(2), '--meta',
                                '--files', '512', '--dirs', '16', '--workers', '16',
                                '--phases', 'mkdir,create', '--keep', '--id', str(iteration)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
            with (work / 'fill.log').open('ab') as f:
                f.write(p.stdout)
            assert p.returncode == 0, p.stdout[-2000:]
        # L0 is full, but no apply/admission watermark has been crossed.
        assert field(st, 'kv_mt_bytes') < field(st, 'kv_mem_budget') * 3 // 4, st
        assert c.leader_placement(2) == {0: 1, 2: 1}, st
        print('REPORT actual L0 cap reached with memtable headroom; load stopped', flush=True)
        write_case('report', m.path / 'report')
        park.unlink()
        end = time.monotonic() + 60
        while field(status(), 'kv_l0_bytes') >= field(st, 'kv_l0_cap'):
            assert time.monotonic() < end, 'compactor did not drain'
            time.sleep(.2)
    if a.case in ('both', 'put'):
        # The storage node remains live. This is never a degraded write.
        for group in (0, 2):
            leader = c.leader_placement(2)[group]
            if leader == 1:
                rc, out = mgmt(env, 'raft-step-down', c.ep(1), str(group), '2')
                assert rc == 0, out
                end = time.monotonic() + 20
                while c.leader_placement(2)[group] == 1:
                    assert time.monotonic() < end, 'leadership did not move'
                    time.sleep(.1)
        lag.touch()
        write_case('put', m.path / 'put')
        lag.unlink()
    for mt in mounts:
        mt.close()
    mounts.clear()
    fresh = Mount(c, 3, 'verify-fresh')
    mounts.append(fresh)
    for name, want in confirmed.items():
        assert (fresh.path / name).read_bytes() == want, f'{name}: fresh-reader mismatch'
    log = (c.dir / 'writer.log').read_text(errors='replace')
    assert 'degraded acknowledgement' not in log, 'live BUSY member treated as down'
    assert 'report-loop' not in log or failures, 'REPORT budget exhausted despite successful close'
finally:
    park.unlink(missing_ok=True)
    lag.unlink(missing_ok=True)
    for mt in mounts:
        mt.close()
    c.stop_all()
    (work / 'result.json').write_text(json.dumps(dict(results=results, failures=failures,
                                                   wall_s=time.monotonic()-t0), indent=2) + '\n')
    print('W166 retained work:', work, flush=True)
assert not failures, '; '.join(failures)
print(f'W166 authority: PASS, {len(confirmed)} exact fresh-reader payloads; '
      f'unchanged 30 s close bound ({time.monotonic()-t0:.2f} s)')
