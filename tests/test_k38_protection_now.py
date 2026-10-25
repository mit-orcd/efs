#!/usr/bin/env python3
"""K38: `efs-mgmt status` reports protection as it is now, and the packer
does not write a segment degraded around a live member.

tests/test_k33_payload.py step 5 read EIO about once in ten runs: 0.3 s
after node 3 returned, the group leader's packer sent a segment fragment to
node 3 on a pooled connection kept across node 3's restart (rc=-6) and wrote
the segment degraded on nodes 4 and 5; `efs-mgmt status` still said
`Degraded segments: 0` and `Protection: full` (the count was the last
rebuild pass's sample), so the test stopped node 4 and the segment's files
had one fragment left.

Real efsd daemons on loopback (tests/k33_daemons.py, the EFS_FAULTS build)
and efs-fuse; private stores under TMPDIR, free ports (never 17xxx). Three
members, one mount through node 1, the packer at a 500 ms minimum age. Fault
hooks, toggled by files in the work directory so each step is forced:
  EFS_FAULT_REBUILD_SAMPLE_HOLD=F   the W154 chunk and K37 segment rebuild
                                    passes stop before they sample or
                                    rebuild while F exists;
  EFS_FAULT_SEGMENT_PUT_REFUSE=DIR  node N refuses segment PUTs while
                                    DIR/refuse-N exists;
  EFS_FAULT_SEGMENT_PUT_STALE=DIR   the packer's next pooled connection to
                                    node N fails once (DIR/stale-N, consumed).
 1. Stale pooled connection: stale-N for a member N that leads no group; a
    batch is packed. The fragment for N is sent again on a new connection
    (`sent on a new connection ... rc=0`), no segment is written degraded,
    `Degraded segments: 0`. (Retry disabled: written degraded.)
 2. Segment marked after the last pass: the passes are held (their sample
    says 0), node N refuses segment PUTs and a batch is packed degraded
    (`written degraded`). Every member's status at once: `Degraded segments:
    >= 1`, `Protection: DEGRADED`. (Base: 0 and `Protection: full`.) Released:
    rebuilt, `Protection: full`.
 3. Chunk acknowledged degraded after the last pass: the passes are held,
    node N stopped, a large file written and fsynced (two fragments). Status
    at once: `Degraded chunks: >= 1`, `Protection: DEGRADED`. (Base: 0 and
    `Protection: full`.) N returns and the hold is released: rebuilt,
    `Protection: full`; a fresh mount reads every file exactly.
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import os, re, shutil, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, Mount, fuse_ok, mgmt, payload  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('K38 protection now: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
work = Path(tempfile.mkdtemp(prefix='efs-k38-', dir=os.environ.get('TMPDIR')))
flags = work / 'flags'
flags.mkdir()
hold = flags / 'sample-hold'
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
env.update(EFS_PACK_MIN_AGE_MS='500', EFS_PACK_INTERVAL_MS='200',
           EFS_FAULT_REBUILD_SAMPLE_HOLD=str(hold), EFS_FAULT_SEGMENT_PUT_REFUSE=str(flags),
           EFS_FAULT_SEGMENT_PUT_STALE=str(flags))
SMALL = (1, 300, 4096, 9000, 16384)
LARGE = 1024 * 1024 + 3
files = {}
t0 = time.time()


def write(m, name, size, sync=False):
    with open(m.path / name, 'wb') as f:
        f.write(payload(name, size))
        if sync:
            f.flush()
            os.fsync(f.fileno())
    files[name] = size


def batch(m, tag):
    for k, size in enumerate(SMALL):
        write(m, f'{tag}-{k}', size)


def logs(c):
    return ''.join(c.log(i) for i in c.ports if (c.dir / f'n{i}.log').exists())


def count(c, pattern):
    return len(re.findall(pattern, logs(c)))


def packed_total(c):
    """Files the packers switched into segments, summed over every daemon
    run and metadata group (as tests/test_k33_payload.py)."""
    total = 0
    for i in c.ports:
        if not (c.dir / f'n{i}.log').exists():
            continue
        for run in c.log(i).split('==== start')[1:]:
            last = {}
            for g, t, n in re.findall(r'packer: group=(\d+) term=(\d+) .*?packed_total=(\d+)', run):
                last[(g, t)] = int(n)
            total += sum(last.values())
    return total


def wait_packed(c, base, n, what, timeout=60):
    end = time.time() + timeout
    while packed_total(c) < base + n:
        assert time.time() < end, f'{what}: packer moved {packed_total(c) - base} of {n}'
        time.sleep(0.2)


def status(c, i):
    return mgmt(c.env, 'status', c.ep(i))[1]


def counts(out):
    """(degraded chunks, degraded segments, protection word) or None."""
    ch = re.search(r'^Degraded chunks: (\d+) \(', out, re.M)
    sg = re.search(r'^Degraded segments: (\d+) \(', out, re.M)
    pr = re.search(r'^Protection: (\S+)', out, re.M)
    return (int(ch.group(1)), int(sg.group(1)), pr.group(1)) if ch and sg and pr else None


def wait_full(c, i, what, timeout=120):
    end, out = time.time() + timeout, ''
    while time.time() < end:
        out = status(c, i)
        if counts(out) == (0, 0, 'full'):
            return
        time.sleep(0.5)
    raise AssertionError(f'{what}: protection not full\n{out}')


def non_leader(c):
    """A member that leads neither metadata group: the packers run on the
    group leaders, so its segment fragments go over the peer pool."""
    led = c.leader_placement(1)
    free = [i for i in (1, 2, 3) if i not in (led.get(0), led.get(2))]
    assert free, led
    return free[-1]


c = Cluster(work, 'k', [1, 2, 3], env)
mounts = []
try:
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    a = Mount(c, 1, 'mnt')
    mounts.append(a)
    base = packed_total(c)
    batch(a, 'b0')
    wait_packed(c, base, len(SMALL), 'baseline')
    wait_full(c, 1, 'baseline')

    # 1. A pooled connection that fails once is not a missing fragment.
    n = non_leader(c)
    degraded0 = count(c, r'written degraded')
    base = packed_total(c)
    (flags / f'stale-{n}').touch()
    batch(a, 's1')
    wait_packed(c, base, len(SMALL), 'stale connection')
    assert not (flags / f'stale-{n}').exists(), 'the packer never used its pooled connection'
    assert count(c, rf'pooled connection to node {n} fails once') == 1, logs(c)[-3000:]
    assert count(c, r'written degraded') == degraded0, \
        'segment written degraded around a live member:\n' + \
        '\n'.join(re.findall(r'.*written degraded.*', logs(c)))
    assert count(c, rf'fragment for node {n} sent on a new connection after the pooled one '
                    rf'failed: rc=0') >= 1, logs(c)[-3000:]
    wait_full(c, 1, 'after the stale connection')
    print(f'1. stale pooled connection to node {n}: fragment sent again on a new connection, '
          'no degraded segment PASS')

    # 2. A segment marked after the last pass is reported at once.
    hold.touch()
    time.sleep(1.5)  # a pass that started before the hold has ended
    n = non_leader(c)
    degraded0 = count(c, r'written degraded')
    base = packed_total(c)
    (flags / f'refuse-{n}').touch()
    batch(a, 's2')
    wait_packed(c, base, len(SMALL), 'refused segment PUT')
    (flags / f'refuse-{n}').unlink()
    assert count(c, r'written degraded') > degraded0, logs(c)[-3000:]
    for i in (1, 2, 3):
        got = counts(status(c, i))
        assert got and got[1] >= 1 and got[2] == 'DEGRADED', \
            f'node {i} status right after a degraded segment write: {got}\n{status(c, i)}'
    hold.unlink()
    wait_full(c, 1, 'segment rebuild')
    print(f'2. segment written degraded (node {n} refused) with the passes held: '
          f'every member reports it at once; rebuilt after release PASS')

    # 3. A chunk acknowledged degraded after the last pass is reported at once.
    n = non_leader(c)
    hold.touch()
    time.sleep(1.5)
    c.stop(n)
    write(a, 'l3', LARGE, sync=True)
    live = [i for i in (1, 2, 3) if i != n]
    for i in live:
        got = counts(status(c, i))
        assert got and got[0] >= 1 and got[2] == 'DEGRADED', \
            f'node {i} status right after a degraded chunk acknowledgement: {got}\n{status(c, i)}'
    assert (a.path / 'l3').read_bytes() == payload('l3', LARGE)
    c.restart(n)
    c.wait_log(n, r'raft-host: up raft_id=\d+ n=3 formed', timeout=60)
    c.wait_members([1, 2, 3], 3)
    hold.unlink()
    wait_full(c, 1, 'chunk rebuild')
    a.close()
    mounts.remove(a)
    d = Mount(c, 2, 'mnt-d')
    mounts.append(d)
    for name, size in files.items():
        assert (d.path / name).read_bytes() == payload(name, size), name
    print(f'3. chunk acknowledged degraded (node {n} down) with the passes held: reported at '
          f'once; rebuilt; {len(files)} files exact through a fresh mount PASS')
finally:
    for m in mounts:
        m.close()
    c.stop_all()
    if os.environ.get('EFS_K38_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print(f'K38 protection now: PASS ({time.time() - t0:.1f} s)')
