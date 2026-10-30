#!/usr/bin/env python3
"""S4/W122 (D46 item 3): `efs-mgmt settings NODE:PORT` and the client listing.

Three real ./efsd daemons on loopback (private stores under TMPDIR, free
ports, discovery off), the product build:
1. Node 1 starts with EFS_PACK_MIN_AGE_MS and EFS_RAFT_APPLIED_INTERVAL_MS
   in the environment and --writers on the command line: the listing gives
   those values with provenance `environment` / `command line`; every other
   tunable and deployment row is `built-in` or `automatic`; node 2, started
   with neither, lists the defaults. Rows are sorted by kind then name; the
   header counts the observability and fault kinds it leaves out.
2. The two cluster policy rows change with `commands-policy set` and
   `expiry-policy cluster set`.
3. Exit status is non-zero for an unreachable node and for a bad address.
4. The client listing (`efs-mgmt settings --mount PATH`) shows the direct-I/O,
   transport and budget rows, and the budget set in the client's environment
   (NOTRUN part without /dev/fuse, fusermount3 or efs-fuse).
"""
import os, re, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import k33_daemons  # noqa: E402
from k33_daemons import Cluster, MGMT, FUSE, mgmt, free_port  # noqa: E402

root = Path(__file__).resolve().parents[1]
k33_daemons.EFSD = root / 'efsd'  # the product daemon, not the fault build
for b in (k33_daemons.EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
work = Path(tempfile.mkdtemp(prefix='efs-settings-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith(('EFS_FAULT', 'EFS_PACK', 'EFS_RAFT', 'EFS_DCACHE'))}


def listing(out):
    """(header, {name: (value, default, provenance, kind)}, [kind order])."""
    lines = out.splitlines()
    assert lines[0].startswith('# daemon settings;') or lines[0].startswith('# client settings;'), out
    rows, order = {}, []
    for l in lines[1:]:
        f = l.split('\t')
        assert len(f) == 5, l
        assert f[0] not in rows, l
        rows[f[0]] = tuple(f[1:])
        order.append((f[4], f[0]))
    assert order == sorted(order), order
    return lines[0], rows


def policies(c, i):
    rc, out = mgmt(env, 'settings', c.ep(i))
    assert rc == 0, out
    _, rows = listing(out)
    return rows['commands-policy'][0], rows['expiry-policy'][0]


c = Cluster(work, 'x', [1, 2, 3], env)
mnt = None
try:
    c.start(1, extra_env={'EFS_PACK_MIN_AGE_MS': '12345', 'EFS_RAFT_APPLIED_INTERVAL_MS': '50'})
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members', timeout=60)

    # 1. Provenance.
    rc, out = mgmt(env, 'settings', c.ep(1))
    assert rc == 0, out
    header, rows = listing(out)
    assert 'observability (34)' in header and 'fault/test hooks (62)' in header, header
    want = {'EFS_PACK_MIN_AGE_MS': ('12345', '60000', 'environment', 'numeric tunable'),
            'EFS_RAFT_APPLIED_INTERVAL_MS': ('50', '100', 'environment', 'numeric tunable'),
            '--writers': ('2', '8', 'command line', 'numeric tunable')}
    for k, v in want.items():
        assert rows.get(k) == v, (k, rows.get(k), out)
    for k, v in rows.items():
        if k not in want:
            assert v[2] in ('built-in', 'automatic', 'policy'), (k, v, out)
    for k in ('EFS_TRANSPORT', 'EFS_RDMA_DEV', 'EFS_QUERY_DISK_TOTAL_MIB', 'transport', 'build',
              'direct-io', 'durability-window', 'commands-policy', 'expiry-policy', 'EFS_PACK_INTERVAL_MS'):
        assert k in rows, (k, out)
    assert not any(k.startswith(('EFS_DCACHE', 'EFS_REPORT_ADMIT')) for k in rows), 'client rows in the daemon'
    assert rows['transport'][0].startswith('tcp') or rows['transport'][0].startswith('rdma'), rows['transport']
    assert rows['build'][0].startswith('efsd'), rows['build']
    _, other = listing(mgmt(env, 'settings', c.ep(2))[1])
    assert other['EFS_PACK_MIN_AGE_MS'][:3] == ('60000', '60000', 'built-in'), other['EFS_PACK_MIN_AGE_MS']
    assert other['EFS_RAFT_APPLIED_INTERVAL_MS'][2] == 'built-in', other['EFS_RAFT_APPLIED_INTERVAL_MS']
    print('settings: environment, command line and built-in provenance on a loopback cluster PASS')

    # 2. The policy rows follow the cluster policies.
    assert policies(c, 1) == ('mask=0 version=0', 'off version=0'), policies(c, 1)
    rc, out = mgmt(env, 'commands-policy', c.ep(1), 'set', '3', '0')
    assert rc == 0 and 'rc=0' in out, out
    rc, out = mgmt(env, 'expiry-policy', c.ep(1), 'cluster', 'set', 'on', '0')
    assert rc == 0, out
    end = time.time() + 30
    while policies(c, 2) != ('mask=3 version=1', 'on version=1'):
        assert time.time() < end, policies(c, 2)
        time.sleep(0.3)
    print('settings: policy rows change with commands-policy set and expiry-policy cluster set PASS')

    # 3. Refusals exit non-zero.
    rc, out = mgmt(env, 'settings', f'127.0.0.1:{free_port()}')
    assert rc != 0, out
    rc, out = mgmt(env, 'settings', 'nonsense')
    assert rc != 0, out
    rc, out = mgmt(env, 'settings')
    assert rc != 0, out
    print('settings: unreachable node and bad arguments exit non-zero PASS')

    # 4. The client listing.
    if FUSE.exists() and os.access('/dev/fuse', os.R_OK | os.W_OK) and subprocess.run(
            ['which', 'fusermount3'], capture_output=True).returncode == 0:
        mnt = work / 'mnt'
        mnt.mkdir()
        log = open(work / 'mnt.log', 'w')
        p = subprocess.Popen([str(FUSE), c.ep(1), str(mnt), '-f'], stdout=log, stderr=subprocess.STDOUT,
                             env=dict(env, EFS_DCACHE_HARD_BYTES='134217728'))
        end = time.time() + 30
        while time.time() < end and not os.path.ismount(mnt):
            assert p.poll() is None, (work / 'mnt.log').read_text()[-2000:]
            time.sleep(0.1)
        assert os.path.ismount(mnt)
        (mnt / 'f').write_bytes(b'x' * 4096)
        rc, out = mgmt(env, 'settings', '--mount', str(mnt))
        assert rc == 0, out
        header, rows = listing(out)
        assert header.startswith('# client settings;'), header
        assert rows['EFS_DCACHE_HARD_BYTES'] == ('134217728', '268435456', 'environment', 'numeric tunable'), rows
        assert rows['EFS_DCACHE_DRAIN_BYTES'][2] == 'built-in', rows
        assert rows['direct-io'][0] == 'on' and rows['build'][0].startswith('efs-fuse'), rows
        assert rows['transport'][0] and not any(k == 'EFS_PACK_MIN_AGE_MS' for k in rows), rows
        rc, out = mgmt(env, 'settings', '--mount', str(work / 'nothing'))
        assert rc != 0, out
        subprocess.run(['fusermount3', '-u', str(mnt)], capture_output=True)
        p.wait(timeout=30)
        mnt = None
        print('settings: client listing shows direct-I/O, transport and budget values PASS')
    else:
        print('settings: client listing NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
finally:
    if mnt is not None:
        subprocess.run(['fusermount3', '-u', str(mnt)], capture_output=True)
    c.stop_all()
import shutil
shutil.rmtree(work, ignore_errors=True)
