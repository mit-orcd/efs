#!/usr/bin/env python3
"""F11/F20: operator workflows on real daemons.

Real efsd daemons on loopback (tests/k33_daemons.py, the EFS_FAULTS build);
private stores under TMPDIR, free ports (never 17xxx). Node addition with
`efsd --join` is covered by tests/test_k33_growth.py and test_k33_autoid.py.

1. Storage roots are fixed when a store is created (F11). A seed started on
   two roots records them in each root's marker. Restarts with a third
   root, with one root, or with the two roots swapped are refused with a
   message and change nothing; the same two roots in order start again.
   `efs-mgmt add-storage` and `add-node` are refused by name.
2. raft-resync (F20): on three members, refused on a group's leader,
   accepted on a follower (raft-status shows resync=1); the follower keeps
   following: after metadata writes through a mount (when FUSE is
   available) its applied index reaches the leader's commit index, and its
   role and leader agree with the group.
3. With FUSE, the F13 POSIX cases (tests/posix/posix_suite.py
   dir_rename_noreplace*, mknod_special_files_refused) on that mount.
"""
import json, os, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, Mount, fuse_ok, mgmt  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
work = Path(tempfile.mkdtemp(prefix='efs-ops-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
t0 = time.time()


def group_status(c, i, group):
    rc, out = mgmt(env, 'raft-status', c.ep(i))
    m = re.search(rf'group {group} hosted=1 role=(\w+) leader=(-?\d+) term=(\d+) '
                  rf'commit=(\d+) applied=(\d+).*?(resync=1)?$', out, re.M)
    assert m, out
    return {'role': m.group(1), 'leader': int(m.group(2)), 'term': int(m.group(3)),
            'commit': int(m.group(4)), 'applied': int(m.group(5)), 'resync': bool(m.group(6))}


mounts = []
stage = 'storage roots'
c = Cluster(work, 'roots', [1], env)
r = Cluster(work, 'resync', [1, 2, 3], env)
try:
    # 1. Fixed storage roots.
    roots = [c.dir / f'root{k}' for k in range(3)]
    for p in roots:
        p.mkdir()
    csv = lambda *ps: ','.join(str(p) for p in ps)  # noqa: E731
    def start_roots(*ps):
        argv = [str(EFSD), c.ep(1), csv(*ps), '--quota', '1G', '--writers', '2',
                '--discovery-port', '0']
        with open(c.dir / 'n1.log', 'a') as log:
            log.write(f'\n==== start {time.time():.3f} {" ".join(argv)}\n')
            log.flush()
            c.procs[1] = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, env=env)

    start_roots(roots[0], roots[1])
    c.wait_log(1, r'node 1 listening on .*\(2 paths')
    rc, out = mgmt(env, 'add-storage', c.ep(1), str(roots[2]))
    assert rc == 2 and 'was removed' in out and 'fixed when its store is created' in out, out
    rc, out = mgmt(env, 'add-node', c.ep(1), c.ep(1))
    assert rc == 2 and 'efsd --join' in out, out
    c.stop(1)
    markers = {p: (p / '.efs-engine').read_bytes() for p in roots[:2]}
    for ps, why in (((roots[0], roots[1], roots[2]), r'1 of 3 roots are new'),
                    ((roots[0],), r'is root 1 of 2 of its root set; this command gives it '
                                  r'as root 1 of 1'),
                    ((roots[1], roots[0]), r'is root 2 of 2 of its root set; this command '
                                           r'gives it as root 1 of 2')):
        start_roots(*ps)
        assert c.wait_exit(1, timeout=30) != 0, c.last_run(1)[-2000:]
        c.wait_log(1, why)
        assert 'fixed when its store is created' in c.last_run(1) or \
            'in the same order' in c.last_run(1), c.last_run(1)[-2000:]
        assert 'listening on' not in c.last_run(1), c.last_run(1)[-2000:]
        assert {p: (p / '.efs-engine').read_bytes() for p in roots[:2]} == markers
        assert not (roots[2] / '.efs-engine').exists(), 'a refused start marked a new root'
    start_roots(roots[0], roots[1])
    c.wait_log(1, r'node 1 listening on .*\(2 paths')
    c.stop(1)
    print('1. storage roots fixed at creation: added, dropped and swapped roots refused; '
          'add-storage and add-node refused; the same roots start PASS')

    # 2. raft-resync on a follower.
    stage = 'resync setup'
    r.start(1)
    r.start(2, join=1)
    r.start(3, join=1)
    for i in (1, 2, 3):
        r.wait_log(i, r'raft-host: formed: 3 members')
    group = 0
    led = r.leader_placement(1)
    leader = led[group]
    follower = next(i for i in (1, 2, 3) if r.node_id(i) != leader)
    lead_i = next(i for i in (1, 2, 3) if r.node_id(i) == leader)
    rc, out = mgmt(env, 'raft-resync', r.ep(lead_i), str(group))
    assert rc == 1 and 'leads the group; refused' in out, out
    rc, out = mgmt(env, 'raft-resync', r.ep(follower), str(group))
    assert rc == 0 and 'requested' in out, out
    r.wait_log(follower, rf'raft: group={group} resync requested')
    if fuse_ok():
        a = Mount(r, 1, 'mnt')
        mounts.append(a)
        for k in range(50):
            stage = f'create f{k}, bytes={k}'
            (a.path / f'f{k}').write_bytes(b'x' * k)
            stage = f'mkdir d{k}'
            os.mkdir(a.path / f'd{k}')
        for k in range(50):
            stage = f'read f{k}, bytes={k}'
            assert (a.path / f'f{k}').read_bytes() == b'x' * k
    stage = 'follower catch-up'
    end = time.time() + 60
    while True:
        lst, fst = group_status(r, lead_i, group), group_status(r, follower, group)
        if (fst['role'] == 'FOLLOWER' and fst['term'] == lst['term'] and
                fst['leader'] == lst['leader'] and fst['applied'] >= lst['commit'] > 0):
            break
        assert time.time() < end, (lst, fst)
        time.sleep(0.5)
    print(f'2. raft-resync: refused on the group {group} leader, accepted on a follower '
          f'(resync={int(fst["resync"])}); the follower applied to commit {lst["commit"]}'
          f'{" after 100 mount operations" if mounts else " (no FUSE: no writes)"} PASS')
    if mounts:
        stage = 'POSIX cases'
        # 3. F13 POSIX cases on this mount: RENAME_NOREPLACE (existing and
        # free target) and mknod of special files.
        suite = Path(__file__).resolve().parent / 'posix/posix_suite.py'
        for filt, cases in (('dir_rename_noreplace', 2), ('mknod_special_files_refused', 1)):
            p = subprocess.run([sys.executable, str(suite), str(mounts[0].path), '--filter', filt],
                               capture_output=True, text=True, timeout=300, env=env)
            assert p.returncode == 0 and f'POSIX suite: {cases}/{cases} pass, 0 fail' in p.stdout, \
                p.stdout + p.stderr
        print('3. POSIX: renameat2(RENAME_NOREPLACE) EEXIST/EOPNOTSUPP, mknod of FIFO, socket '
              'and devices EPERM PASS')
except Exception as error:
    # Capture before shutdown; a successful rerun cannot reconstruct the
    # failed operation's Raft roles, import state or client log.
    diagnostic = {'stage': stage, 'error': repr(error), 'elapsed_s': time.time() - t0,
                  'nodes': {}}
    for i in (1, 2, 3):
        try:
            rc, out = mgmt(env, 'raft-status', r.ep(i))
            diagnostic['nodes'][str(i)] = {'endpoint': r.ep(i), 'rc': rc, 'status': out}
        except Exception as status_error:
            diagnostic['nodes'][str(i)] = {'error': repr(status_error)}
    (work / 'failure.json').write_text(json.dumps(diagnostic, indent=2) + '\n')
    print(f'operator failure: {stage}: {error!r}; evidence {work}', flush=True)
    raise
finally:
    failed = sys.exc_info()[0] is not None
    for m in mounts:
        m.close()
    c.stop_all()
    r.stop_all()
    if failed or os.environ.get('EFS_OPS_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print(f'operator workflows: fixed storage roots, removed commands, raft-resync PASS '
      f'({time.time() - t0:.1f} s)')
