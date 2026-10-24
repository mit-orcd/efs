#!/usr/bin/env python3
"""K33 acceptance: additions interrupted at every durable handoff.

Real efsd daemons on loopback (tests/k33_daemons.py), private stores under
TMPDIR, free ports (never 17xxx), discovery off. The EFS_FAULT_MEMBER_EXIT
hooks end a daemon (status 86) at one handoff of
docs/status/k33-membership-protocol-20261023.md M9; with
EFS_FAULT_MEMBER_EXIT_ONCE the hook is given to every member and only the
first one to reach the point ends (the leader of the step, whichever node
that is). After every interruption the ended daemon is restarted (or the
join retried) and the test checks one membership: every member reports the
same table of ids 1..N with distinct endpoints, each daemon holds the id its
endpoint has in that table, group 0 votes on {1,2,3} and group 2 on {2,3,4}
from four members. Packing is off (EFS_PACK_MIN_AGE_MS one hour) and GC is
off, so the data check is exact: files written before the additions read
back, and the fragment files on all stores are exactly three per chunk of
the files written (no orphaned fragment on any store, none on a joiner).

A. Group-2 leader ends with node 4 a learner, not caught up
   (learner-added); restarted, the change completes. Node 5 ends after
   persisting the record naming it (joiner after-learn); restarted it is
   member 5. Node 4 ends after persisting revision 4 (member after-learn)
   while node 6 joins; restarted, it holds 6 members. Two joins at once,
   both interrupted: node 7 through node 2, which ends right after
   committing a revision (after-members-commit), and node 8 forwarded by
   storage-only node 5, ending when the record naming it arrives
   (before-learn); the proposer restarts, node 8 retries, ids 7 and 8.
B. Group-2 leader ends with JOINT {1,2,3}+{2,3,4} appended, not committed
   (joint-started); the next leader finishes JOINT and COLD.
C. Group-0 leader ends after the revision is on group 2, before it notes
   the copy (after-g2-copy); the next leader copies again (history =
   success) and the voters move.
Exact payloads through FUSE when available (NOTRUN otherwise; the
membership checks still run).
"""
import os, shutil, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import (Cluster, EFSD, MGMT, Mount, fuse_ok, payload,  # noqa: E402
                         fragments, voters_ok)

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
work = Path(tempfile.mkdtemp(prefix='efs-k33i-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_MEMBER_EXIT')}
env.update(EFS_FAULT_GC_DISABLE='1', EFS_PACK_MIN_AGE_MS='3600000')
CHUNK = 128 * 1024
SIZES = (1, 4096, 100000, CHUNK, 3 * CHUNK + 17)
FUSE = fuse_ok()
notes = []
clusters, mounts = [], []


def hook(point):
    return {'EFS_FAULT_MEMBER_EXIT': point, 'EFS_FAULT_MEMBER_EXIT_ONCE': str(work / 'once')}


def form(name, n, member_env=None):
    c = Cluster(work, name, list(range(1, n + 1)), env)
    clusters.append(c)
    c.start(1, extra_env=member_env)
    c.start(2, join=1, extra_env=member_env)
    c.start(3, join=1, extra_env=member_env)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    c.wait_voters([1, 2, 3], lambda i, v: v == {0: (1, 7), 2: (1, 7)})
    return c


def write_files(c, node, tag):
    if not FUSE:
        return {}
    m = Mount(c, node, f'mnt-{tag}')
    files = {}
    for k, size in enumerate(SIZES):
        name = f'{tag}-{k}'
        (m.path / name).write_bytes(payload(name, size))
        files[name] = size
    os.sync()
    m.close()
    return files


def check(c, live, count, files):
    """One membership of `count`, ids by endpoint, voters, exact data and
    no orphaned fragment."""
    tab = c.wait_members(live, count, timeout=90)
    for i in live:
        assert tab[c.node_id(i)] == c.ep(i), (i, c.node_id(i), tab)
    assert len({c.node_id(i) for i in live}) == len(live)
    c.wait_voters(live, lambda i, v: voters_ok(c.node_id(i), v, count), timeout=90)
    if not files:
        return
    m = Mount(c, live[-1], f'mnt-check-{count}-{len(mounts)}')
    mounts.append(m)
    for name, size in files.items():
        assert (m.path / name).read_bytes() == payload(name, size), name
    m.close()
    mounts.remove(m)
    want = 3 * sum((size + CHUNK - 1) // CHUNK for size in files.values())
    got = {i: fragments(c.dir / f'n{i}') for i in c.ports if (c.dir / f'n{i}').exists()}
    assert sum(got.values()) == want, (want, got)


def interrupted(c, ids, point, timeout=60):
    i, rc = c.wait_any_exit(ids, timeout)
    assert rc == 86, (i, rc, c.last_run(i)[-2000:])
    c.wait_log(i, f'EFS_FAULT_MEMBER_EXIT at {point}')
    return i


t0 = time.time()
try:
    # A. learner-added, joiner after-learn, member after-learn, concurrent.
    c = form('a', 8, hook('learner-added'))
    files = write_files(c, 1, 'a0')
    c.start(4, join=1)
    x = interrupted(c, [1, 2, 3], 'learner-added')
    notes.append(f'A learner-added: group-2 leader node {x} ended')
    c.restart(x)
    c.wait_log(4, r'raft-host: up raft_id=3 n=4 formed', run=False)
    check(c, [1, 2, 3, 4], 4, files)
    files.update(write_files(c, 2, 'a1'))

    c.start(5, join=2, extra_env={'EFS_FAULT_MEMBER_EXIT': 'after-learn'})
    assert interrupted(c, [5], 'after-learn') == 5
    assert 'Joined cluster' not in c.last_run(5)
    c.restart(5)
    c.wait_log(5, r'raft-host: up raft_id=4 n=5 formed')
    check(c, [1, 2, 3, 4, 5], 5, files)

    c.stop(4)
    c.restart(4, extra_env={'EFS_FAULT_MEMBER_EXIT': 'after-learn'})
    c.wait_log(4, r'raft-host: up raft_id=3 n=5 formed')
    c.start(6, join=3)
    assert interrupted(c, [4, 6], 'after-learn') == 4
    c.wait_log(6, r'raft-host: up raft_id=5 n=6 formed')
    c.restart(4)
    c.wait_log(4, r'raft-host: up raft_id=3 n=6 formed')
    check(c, [1, 2, 3, 4, 5, 6], 6, files)

    c.stop(2)
    c.restart(2, extra_env={'EFS_FAULT_MEMBER_EXIT': 'after-members-commit'})
    c.wait_log(2, r'raft-host: up raft_id=1 n=6 formed')
    c.wait_voters([1, 2, 3], lambda i, v: v[0] == (1, 7))
    c.start(7, join=2)
    c.start(8, join=5, extra_env={'EFS_FAULT_MEMBER_EXIT': 'before-learn'})
    ended = set()
    while ended != {2, 8}:
        i, rc = c.wait_any_exit([k for k in (2, 7, 8) if k not in ended], 90)
        assert i in (2, 8) and rc == 86, (i, rc, c.last_run(i)[-2000:])
        ended.add(i)
    c.wait_log(2, r'EFS_FAULT_MEMBER_EXIT at after-members-commit')
    c.wait_log(8, r'EFS_FAULT_MEMBER_EXIT at before-learn')
    c.restart(2)
    c.start(8, join=5)
    for i in (7, 8):
        c.wait_log(i, r'raft-host: up raft_id=\d+ n=[78] formed', timeout=90)
    assert sorted((c.node_id(7), c.node_id(8))) == [7, 8]
    check(c, list(range(1, 9)), 8, files)
    notes.append(f'A concurrent: node 7 -> id {c.node_id(7)}, node 8 -> id {c.node_id(8)}')
    c.stop_all()

    # B. joint-started.
    c = form('b', 4, hook('joint-started'))
    files = write_files(c, 1, 'b0')
    c.start(4, join=1)
    x = interrupted(c, [1, 2, 3], 'joint-started')
    assert x in (2, 3), x  # node 1 is retired from group 2 at four members
    notes.append(f'B joint-started: group-2 leader node {x} ended')
    c.restart(x)
    check(c, [1, 2, 3, 4], 4, files)
    c.stop_all()

    # C. after-g2-copy.
    c = form('c', 5, hook('after-g2-copy'))
    files = write_files(c, 1, 'c0')
    c.start(4, join=3)
    x = interrupted(c, [1, 2, 3], 'after-g2-copy')
    notes.append(f'C after-g2-copy: group-0 leader node {x} ended')
    c.restart(x)
    check(c, [1, 2, 3, 4], 4, files)
    c.start(5, join=4)
    c.wait_log(5, r'raft-host: up raft_id=4 n=5 formed')
    check(c, [1, 2, 3, 4, 5], 5, files)
finally:
    for m in mounts:
        m.close()
    for c in clusters:
        c.stop_all()
    if os.environ.get('EFS_K33_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
if not FUSE:
    notes.append('NOTRUN FUSE payload checks (no /dev/fuse, fusermount3 or efs-fuse)')
for n in notes:
    print(n)
print('K33 interrupted additions: learner-added, joint-started, after-g2-copy, after-learn '
      '(joiner and member), after-members-commit and before-learn in concurrent joins: one '
      f'membership, distinct ids, exact data, no orphaned fragments: PASS ({time.time() - t0:.1f} s)')
