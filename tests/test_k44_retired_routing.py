#!/usr/bin/env python3
"""K44/W167: a retired seed still routes management and client requests.

Forms three members, adds a fourth online, proves seed g2 retirement, then
checks the seed without application retries. --host/--port-min select only
private test endpoints for the ORCD TCP/RDMA stages; they change no product
behavior. --out retains closed logs/results outside Git (including failures).
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import k33_daemons as kd

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--host', default='127.0.0.1')
p.add_argument('--port-min', type=int)
p.add_argument('--out', type=Path)
a = p.parse_args()
if a.port_min:
    assert 19000 <= a.port_min <= 19990
    def private_ports(n):
        ports = []
        for port in range(a.port_min, 20000):
            with socket.socket() as s:
                try:
                    s.bind((a.host, port))
                except OSError:
                    continue
            ports.append(port)
            if len(ports) == n:
                return ports
        raise AssertionError('no private ports available')
    kd.free_ports = private_ports
work = Path(tempfile.mkdtemp(prefix='efs-k44-', dir=os.environ.get('TMPDIR')))
env = dict(os.environ)
env.pop('EFS_FAULT_MEMBER_EXIT', None)
c = kd.Cluster(work, 'retired', [1, 2, 3, 4], env,
               hosts={i: a.host for i in range(1, 5)})
rows = []
mount = None
mount_log = None
passed = False

def command(label, *args):
    begin = time.monotonic()
    rc, out = kd.mgmt(env, *args)
    rows.append(dict(command=label, argv=list(args), rc=rc, output=out,
                     seconds=round(time.monotonic()-begin, 3)))
    print(json.dumps(rows[-1]), flush=True)
    return rc, out

try:
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    c.start(4, join=1)
    c.wait_voters([2, 3, 4], lambda i, v: v[2] == (1, 0xe))
    c.wait_log(1, r'group 2 replica retired')
    rc, status = command('raft-status', 'raft-status', c.ep(1))
    assert rc == 0 and re.search(r'group 2 .*retired=1', status), status
    # Record the whole sweep before raising the decisive MKFS failure.
    mkfs_rc, mkfs_out = command('raft-mkfs', 'raft-mkfs', c.ep(1))
    rc, out = command('commands-policy get', 'commands-policy', c.ep(1), 'get')
    assert rc == 0, out
    version = re.search(r'version=(\d+)', out).group(1)
    mask = re.search(r'mask=(\d+)', out).group(1)
    policy_rc, policy_out = command('commands-policy set', 'commands-policy', c.ep(1),
                                    'set', mask, version)
    expiry_rc, expiry_out = command('expiry-policy', 'expiry-policy', c.ep(1), 'cluster', 'get')
    status_rc, status_out = command('status', 'status', c.ep(1))
    compact_rc, compact_out = command('pack-compact summary', 'pack-compact', c.ep(1), '0')
    for cmd in ('sync-status', 'data-commit', 'io-stats', 'version', 'gc-status', 'expiry-status'):
        rc, out = command(cmd, cmd, c.ep(1))
        assert rc == 0, out
    for cmd, args in (('raft-getattr', ['1']), ('raft-readdir', ['1']),
                      ('raft-lookup-path', ['/']), ('expiry-clock', ['get'])):
        rc, out = command(cmd, cmd, c.ep(1), *args)
        assert rc == 0, out
    # No membership mutation: ask for the already committed voter set.
    rc, out = command('raft-change same voters', 'raft-change', c.ep(1), '2', '0xe')
    assert rc == 0, out
    step_rc, step_out = command('raft-step-down retired', 'raft-step-down', c.ep(1), '2')
    resync_rc, resync_out = command('raft-resync retired', 'raft-resync', c.ep(1), '2')
    rc, out = command('raft-setattr', 'raft-setattr', c.ep(1), '1', '6', '0',
                       str(os.getuid()), str(os.getgid()))
    assert rc == 0, out
    if kd.FUSE.exists() and os.access('/dev/fuse', os.R_OK | os.W_OK) and shutil.which('fusermount3'):
        mnt = work/'mnt'; mnt.mkdir()
        mount_log = open(work/'mount.log', 'w')
        mount = subprocess.Popen([str(kd.FUSE), c.ep(1), str(mnt), '-f'], env=env,
                                 stdout=mount_log, stderr=subprocess.STDOUT)
        end = time.monotonic()+30
        while not os.path.ismount(mnt) and time.monotonic() < end and mount.poll() is None:
            time.sleep(0.05)
        assert os.path.ismount(mnt), (work/'mount.log').read_text()[-3000:]
        payload = bytes(range(256))*4097
        with (mnt/'seed-only').open('wb') as f:
            f.write(payload); f.flush(); os.fsync(f.fileno())
        assert (mnt/'seed-only').read_bytes() == payload
        ino = str((mnt/'seed-only').stat().st_ino)
        for cmd, args in (('raft-lookup', ['1', 'seed-only']),
                          ('raft-getattr', [ino]), ('raft-getchunks', [ino])):
            rc, out = command(cmd, cmd, c.ep(1), *args)
            assert rc == 0, out
        os.chmod(mnt/'seed-only', 0o640)
        os.setxattr(mnt/'seed-only', 'user.k44', b'exact')
        assert os.getxattr(mnt/'seed-only', 'user.k44') == b'exact'
        os.removexattr(mnt/'seed-only', 'user.k44')
        with (mnt/'seed-only').open('r+b') as f:
            fcntl.flock(f, fcntl.LOCK_EX)
            fcntl.lockf(f, fcntl.LOCK_EX, 64)
            fcntl.lockf(f, fcntl.LOCK_UN, 64)
            fcntl.flock(f, fcntl.LOCK_UN)
            f.truncate(4099); f.flush(); os.fdatasync(f.fileno())
        assert (mnt/'seed-only').read_bytes() == payload[:4099]
        payload = payload[:4099]
        os.link(mnt/'seed-only', mnt/'hardlink')
        assert (mnt/'hardlink').read_bytes() == payload
        (mnt/'hardlink').unlink()
        (mnt/'symlink').symlink_to('seed-only')
        assert os.readlink(mnt/'symlink') == 'seed-only'
        assert (mnt/'symlink').read_bytes() == payload
        (mnt/'symlink').unlink()
        assert os.statvfs(mnt).f_blocks > 0
        (mnt/'directory').mkdir()
        assert sorted(x.name for x in mnt.iterdir()) == ['directory', 'seed-only']
        (mnt/'seed-only').rename(mnt/'renamed')
        assert (mnt/'renamed').read_bytes() == payload
        (mnt/'renamed').unlink(); (mnt/'directory').rmdir()
        print('seed-only mount: create/fsync/read/getattr/readdir/rename/unlink/mkdir/rmdir PASS', flush=True)
    else:
        print('NOTRUN seed-only FUSE checks: missing FUSE prerequisites', flush=True)
    assert mkfs_rc == 0, 'K44 retired-seed raft-mkfs: '+mkfs_out
    assert policy_rc == 0, policy_out
    assert expiry_rc == status_rc == compact_rc == 0, (expiry_out, status_out, compact_out)
    assert step_rc != 0 and 'rc=-15' in step_out, step_out
    assert resync_rc != 0 and 'rc=-15' in resync_out, resync_out
    # An administrative refusal must not schedule resync on a retired replica.
    rc, after = command('raft-status after', 'raft-status', c.ep(1))
    assert rc == 0, after
    retired = re.search(r'group 2 .*', after).group(0)
    assert 'resync=1' not in retired, retired
    assert re.search(r'leader=[123] ', retired), retired
    for field in ('term', 'commit', 'applied', 'last'):
        old = re.search(r'group 2 .*', status).group(0)
        assert re.search(r'\b'+field+r'=(\d+)', old).group(1) == re.search(
            r'\b'+field+r'=(\d+)', retired).group(1), (old, retired)
    passed = True
finally:
    if mount:
        if os.path.ismount(work/'mnt'):
            subprocess.run(['fusermount3', '-u', str(work/'mnt')], check=True)
        try:
            mount.wait(timeout=30)
        except subprocess.TimeoutExpired:
            mount.kill(); mount.wait()
    if mount_log:
        mount_log.close()
    c.stop_all()
    if a.out:
        a.out.mkdir(parents=True, exist_ok=True)
        (a.out/'commands.json').write_text(json.dumps(rows, indent=2)+'\n')
        for f in work.rglob('*.log'):
            shutil.copyfile(f, a.out/('-'.join(f.relative_to(work).parts)))
        (a.out/'fixture.txt').write_text(str(work)+'\n')
    if passed and not a.out:
        shutil.rmtree(work)
    else:
        print('kept fixture', work, flush=True)
print('K44 retired-seed routing PASS', flush=True)
