#!/usr/bin/env python3
"""K41: the packer never places a segment fragment on a member the cluster
knows is down, and a destination that fails at write time is replaced by a
live spare instead of being written degraded.

Real efsd daemons on loopback (tests/k33_daemons.py, the EFS_FAULTS build)
and efs-fuse; private stores under TMPDIR, free ports (never 17xxx). Five
members, one mount through node 1, small files (300 B to 16 KiB) packed by
the server packer (20 s minimum age, so a batch is packed after the faults
below are in place). Fault hooks:
  EFS_FAULT_PACK_PLACE_DOWN=6       the packer counts node 6 as live even
                                    while it is marked down, so its
                                    placement probe meets a silent member;
  EFS_FAULT_SEGMENT_PUT_REFUSE=DIR  node N refuses segment PUTs while
                                    DIR/refuse-N exists.

 1. Member down across a membership change with a silent member: a batch is
    written, node 4 stops, node 6 is added and then stopped with SIGSTOP (it
    holds no data; its socket accepts connections, nothing answers). Every
    running member marks node 4 and node 6 down within 30 s: before K41 the
    heartbeat probed members one after another with 30 s timeouts, so a
    silent member held each cycle for 30 s and the group-0 leader of
    tests/test_k33_growth.py never marked a stopped member (its packer
    placed a fragment there). The batch is then packed: every new segment
    has three fragments on running members (none on 4 or 6), node 6 failed
    the packer's placement probe and was replaced by a spare, no segment was
    written degraded, `Degraded segments: 0`.
 2. Refusal at write time: node 6 resumes and refuses segment PUTs, node 4
    returns. A batch is packed: the packer cancels each admission whose
    fragment node 6 refused and places the files again on live spares; every
    new segment has three fragments, none on node 6, nothing degraded.
 3. Any single loss: with every member back, a holder of a step-1 segment
    stops and a fresh mount reads every file exactly.
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import os, re, signal, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, SLOW, Mount, fuse_ok, mgmt, payload  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('K41 packer placement: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
work = Path(tempfile.mkdtemp(prefix='efs-k41-', dir=os.environ.get('TMPDIR')))
flags = work / 'flags'
flags.mkdir()
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
env.update(EFS_PACK_MIN_AGE_MS='20000', EFS_PACK_INTERVAL_MS='200',
           EFS_PACK_COMPACT_IDLE_S='1000000',
           # the daily compaction window (2 h) starts 12 h from now
           EFS_PACK_COMPACT_DAILY_START_MIN=str((time.gmtime().tm_hour * 60 + time.gmtime().tm_min
                                                 + 720) % 1440),
           EFS_FAULT_PACK_PLACE_DOWN='6', EFS_FAULT_SEGMENT_PUT_REFUSE=str(flags))
SIZES = (300, 4096, 9000, 12000, 16384)
BATCH = 60  # about 12 segments of 128 KiB over the two groups
files = {}
t0 = time.time()


def write(m, name, size):
    (m.path / name).write_bytes(payload(name, size))
    files[name] = size


def batch(m, tag):
    names = [f'{tag}-{k}' for k in range(BATCH)]
    for k, name in enumerate(names):
        write(m, name, SIZES[k % len(SIZES)])
    return names


def status(c, i):
    return mgmt(c.env, 'status', c.ep(i))[1]


def logs(c, ids):
    return ''.join(c.log(i) for i in ids if (c.dir / f'n{i}.log').exists())


def count(c, pattern, ids=range(1, 7)):
    return len(re.findall(pattern, logs(c, ids)))


def packer_stats(c):
    """Node-wide packer counters summed over the running members:
    {files_packed, put_errors, spares, degraded}."""
    alive = [i for i in sorted(c.procs) if c.alive(i)]
    rc, out = mgmt(c.env, 'pack-compact', c.ep(alive[0]), '0')
    got = re.findall(r'files_packed=(\d+) .*?put_errors=(\d+) .*?spares=(\d+) degraded=(\d+)', out)
    assert got, out
    keys = ('files_packed', 'put_errors', 'spares', 'degraded')
    return {k: sum(int(g[n]) for g in got) for n, k in enumerate(keys)}


def wait_packed(c, base, n, what, timeout=150):
    end = time.time() + timeout * SLOW
    while packer_stats(c)['files_packed'] < base + n:
        assert time.time() < end, f'{what}: packer moved {packer_stats(c)["files_packed"] - base} ' \
            f'of {n}\n{status(c, 1)}\n{logs(c, (1, 2, 3))[-3000:]}'
        time.sleep(0.5)


def packed_logged(c, ids):
    """files_packed of the running daemons in ids from their packer pass
    lines (packed_total, node-wide since start). Read from the logs: every
    efs-mgmt query waits for each member's answer, 30 s for a silent one."""
    total = 0
    for i in ids:
        got = re.findall(r'packer: group=\d+ .*? packed_total=(\d+)', c.last_run(i))
        total += int(got[-1]) if got else 0
    return total


def wait_packed_logged(c, ids, base, n, what, timeout=150):
    end = time.time() + timeout * SLOW
    while packed_logged(c, ids) < base + n:
        assert time.time() < end, f'{what}: packer moved {packed_logged(c, ids) - base} of {n}\n' \
            f'{logs(c, ids)[-3000:]}'
        time.sleep(0.5)


def segments(c, ids=range(1, 7)):
    """{(group, id, gen): {fragment: node}} over every store's segment files."""
    out = {}
    for i in ids:
        for p in (c.dir / f'n{i}' / 'data' / 'segments').glob('g*/*/*'):
            m = re.match(r'^([0-9a-f]{16})\.([0-9a-f]+)\.(\d)$', p.name)
            if m:
                key = (int(p.parent.parent.name[1:]), int(m.group(1), 16), int(m.group(2), 16))
                out.setdefault(key, {})[int(m.group(3))] = i
    return out


def degraded_lines(c):
    return re.findall(r'packer: segment g\d+ id=[0-9a-f]+ gen=[0-9a-f]+ written degraded', logs(c, range(1, 7)))


def degraded_status(c, i):
    m = re.search(r'^Degraded segments: (\d+)', status(c, i), re.M)
    assert m, status(c, i)
    return int(m.group(1))


def cancelled(c):
    """(group, id, gen) of every admission the packer cancelled to place its
    files again: its fragments stay until reclaim removes them."""
    return {(int(g), int(i, 16), int(n, 16)) for g, i, n in
            re.findall(r'packer: segment g(\d+) id=([0-9a-f]+) gen=([0-9a-f]+) fragment \d+ for node \d+ '
                       r'not acknowledged \(rc=-?\d+\); cancelled', logs(c, range(1, 7)))}


def check_new(c, before, live, what):
    """Every segment not in `before` (nor cancelled): three fragments on
    distinct members of `live`. Returns the new segments."""
    gone = cancelled(c)
    new = {k: v for k, v in segments(c).items() if k not in before and k not in gone}
    assert new, f'{what}: no new segment'
    for key, frags in new.items():
        assert sorted(frags) == [0, 1, 2], f'{what}: segment {key} fragments {frags}'
        holders = set(frags.values())
        assert len(holders) == 3 and holders <= set(live), f'{what}: segment {key} on {frags}, live {live}'
    return new


def wait_up(c, n, timeout=90):
    end = time.time() + timeout * SLOW
    while f'({n} up, 0 down)' not in status(c, 1):
        assert time.time() < end, status(c, 1)
        time.sleep(0.5)


def verify(c, node, names, what):
    m = Mount(c, node, f'v{int(time.time() * 1000)}')
    try:
        for name in names:
            got = (m.path / name).read_bytes()
            assert got == payload(name, files[name]), f'{what}: {name} differs ({len(got)} of {files[name]} B)'
    finally:
        m.close()


c = Cluster(work, 'k', [1, 2, 3, 4, 5, 6], env)
mounts = []
stopped = None
try:
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    c.start(4, join=1)
    c.wait_members([1, 2, 3, 4], 4)
    c.start(5, join=1)
    c.wait_members([1, 2, 3, 4, 5], 5)
    st5 = c.status(5)
    assert not any(serves for serves, _ in st5.values()), f'node 5 hosts a group: {st5}'
    a = Mount(c, 1, 'mnt')
    mounts.append(a)

    # 1. Node 4 down, node 6 added and silent, then the packer runs. The
    # counters are taken after the stop: they are per daemon since start.
    before = segments(c)
    probes0 = count(c, r'fragment \d: node 6 failed the placement probe; placed on spare node')
    names2 = batch(a, 'down')
    t_stop = time.time()
    c.stop(4)
    s1 = packer_stats(c)
    base = packed_logged(c, (1, 2, 3, 5))
    c.start(6, join=1)
    c.wait_log(6, r'raft-host: up raft_id=\d+ n=6 formed', timeout=60)
    c.wait_members([1, 2, 3, 5, 6], 6)
    c.procs[6].send_signal(signal.SIGSTOP)
    stopped = 6
    t_silent = time.time()
    for node, since in ((4, t_stop), (6, t_silent)):
        end = since + 30 * SLOW
        for i in (1, 2, 3, 5):
            pat = rf'heartbeat: node {node} \(\S+\) unresponsive — marked down'
            while not re.search(pat, c.last_run(i)):
                assert time.time() < end, f'node {i} never marked node {node} down within 30 s\n' \
                    f'{c.last_run(i)[-3000:]}'
                time.sleep(0.2)
    print(f'1a. node 4 stopped and node 6 added then silent: nodes 1, 2, 3, 5 mark both down '
          f'({time.time() - t_stop:.1f} s after the stop) PASS')
    wait_packed_logged(c, (1, 2, 3, 5), base, BATCH, 'node 4 down, node 6 silent')
    new2 = check_new(c, before, (1, 2, 3, 5), 'node 4 down, node 6 silent')
    probes = count(c, r'fragment \d: node 6 failed the placement probe; placed on spare node') - probes0
    assert probes >= 1, logs(c, (1, 2, 3))[-4000:]
    assert not degraded_lines(c), degraded_lines(c)
    assert not re.search(r'fragment \d+ for node [46] not acknowledged', logs(c, (1, 2, 3, 5)))
    (flags / 'refuse-6').write_text('')  # step 2: node 6 refuses from the moment it resumes
    c.procs[6].send_signal(signal.SIGCONT)
    stopped = None
    s2 = packer_stats(c)
    assert s2['degraded'] == s1['degraded'] and degraded_status(c, 1) == 0, (s2, status(c, 1))
    print(f'1b. packed with node 4 down and node 6 silent: {len(new2)} segments, three fragments on '
          f'running members each, node 6 replaced after {probes} failed probes, nothing degraded PASS '
          f'({time.time() - t0:.0f} s)')

    # 2. A destination refusing at write time; node 4 returns meanwhile.
    s3 = packer_stats(c)
    before = segments(c)
    refused0 = count(c, r'fragment \d for node 6 not acknowledged \(rc=-\d+\); cancelled, placed again on live spares')
    names3 = batch(a, 'refuse')
    c.restart(4)
    wait_packed(c, s3['files_packed'], BATCH, 'node 6 refusing')
    (flags / 'refuse-6').unlink()
    new3 = check_new(c, before, (1, 2, 3, 4, 5), 'node 6 refusing')
    s4 = packer_stats(c)
    refused = count(c, r'fragment \d for node 6 not acknowledged \(rc=-\d+\); cancelled, placed again '
                       r'on live spares') - refused0
    assert refused >= 1, logs(c, (1, 2, 3))[-4000:]
    assert s4['spares'] >= s3['spares'] + refused, (s3, s4)
    assert not degraded_lines(c) and s4['degraded'] == s3['degraded'], degraded_lines(c)
    assert degraded_status(c, 1) == 0, status(c, 1)
    print(f'2. node 6 refused segment PUTs: {refused} admissions cancelled and placed again on '
          f'spares, {len(new3)} segments with three fragments, none on node 6, nothing degraded PASS '
          f'({time.time() - t0:.0f} s)')

    # 3. Any single loss after the faults.
    wait_up(c, 6)
    holder = next(n for frags in new2.values() for n in frags.values() if n != 1)
    c.stop(holder)
    verify(c, 1, names2 + names3, f'node {holder} down')
    print(f'3. node {holder} (a holder of step-1 segments) down: every file reads back exactly PASS')
    print(f'K41 packer placement: heartbeat marks survive membership changes and silent members, '
          f'segments placed on live members, refusals replaced by spares: PASS '
          f'({time.time() - t0:.1f} s)')
finally:
    if stopped and stopped in c.procs:
        c.procs[stopped].send_signal(signal.SIGCONT)
    for m in mounts:
        m.close()
    c.stop_all()
    if os.environ.get('EFS_K41_KEEP'):
        print(f'kept {work}')
    else:
        import shutil
        shutil.rmtree(work, ignore_errors=True)
