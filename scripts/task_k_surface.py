#!/usr/bin/env python3
"""Task K supplemental CLI surface and unchanged POSIX core at a retired seed.

Run after the code gate, with the already built product. Records diagnostic
precondition refusals separately; process exit zero is never the sole oracle.
No product options, deadlines, formats or acceptance thresholds are changed.
"""
import argparse
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

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root/'tests'))
import k33_daemons as kd
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--host', default='127.0.0.1')
p.add_argument('--port-min', type=int, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--product-source', required=True)
a = p.parse_args()
assert 19000 <= a.port_min <= 19990
version = subprocess.check_output([str(root/'efsd'), '--version'], text=True)
assert a.product_source in version, version

def ports(n):
    out = []
    for port in range(a.port_min, 20000):
        with socket.socket() as s:
            try:
                s.bind((a.host, port))
            except OSError:
                continue
        out.append(port)
        if len(out) == n:
            return out
    raise AssertionError('no private ports free')
kd.free_ports = ports
a.out.mkdir(parents=True, exist_ok=False)
left = []
for proc in Path('/proc').glob('[0-9]*'):
    try:
        if proc.stat().st_uid == os.getuid() and (proc/'exe').resolve() in (
                root/'efsd', kd.EFSD, kd.FUSE):
            left.append(str(proc))
    except OSError:
        pass
left.extend(x for x in Path('/proc/self/mountinfo').read_text().splitlines()
            if str(root/'tmp')+'/' in x)
if left:
    (a.out/'blocked.json').write_text(json.dumps(left, indent=2)+'\n')
    sys.exit('NOTRUN: predecessor fixture processes/mounts remain: '+repr(left))
work = Path(tempfile.mkdtemp(prefix='efs-k44-surface-', dir=os.environ.get('TMPDIR')))
env = dict(os.environ)
c = kd.Cluster(work, 'surface', [1, 2, 3, 4], env,
               hosts={i: a.host for i in range(1, 5)})
rows, failures = [], []
mount = None

def call(cmd, *args, expected='ok'):
    begin = time.monotonic()
    rc, out = kd.mgmt(env, cmd, c.ep(1), *map(str, args))
    statuses = [int(x) for x in re.findall(r'\bstatus=(\d+)', out)]
    values = [int(x) for x in re.findall(r'\brc=(-?\d+)', out)]
    if expected == 'ok':
        good = rc == 0 and all(x == 0 for x in statuses + values)
    elif expected == 'expiry-off':
        good = rc != 0 and (-16 in values or 'status -16' in out or 'policy is off' in out)
    elif expected == 'dir-precondition':
        good = values and values[-1] in (0, -1)
    elif expected == 'unidentified-report':
        good = statuses and statuses[-1] == 15  # EFS_INODE_RPC_UNREGISTERED: required session identity absent
    else:
        raise AssertionError(expected)
    row = dict(command=cmd, args=list(map(str,args)), exit=rc, statuses=statuses,
               verdicts=values, expected=expected, passed=bool(good), output=out,
               seconds=round(time.monotonic()-begin, 3))
    rows.append(row);print(json.dumps(row), flush=True)
    if not good:
        failures.append(row)
    return out

try:
    c.start(1);c.start(2,join=1);c.start(3,join=1)
    for i in (1,2,3):c.wait_log(i,r'raft-host: formed: 3 members')
    c.start(4,join=1)
    c.wait_voters([2,3,4],lambda i,v:v[2]==(1,14))
    c.wait_log(1,r'group 2 replica retired')
    call('raft-setattr','1','6','0',os.getuid(),os.getgid())
    call('mkfs')  # alias of the same idempotent operation, no wipe
    call('shrink-quota','1M')  # private node 1 only, data remains far below quota
    out=call('raft-create','1','cli-file','0100600')
    found=re.search(r'\bino=(\d+)',out);assert found and int(found.group(1)),out
    ino=found.group(1);shard=str(int(ino)&4095)
    uuid='45'+'44'*15  # session authority hashes to the even metadata group
    for op,args in [('create',['1']),('register',[shard,'1']),
                    ('establish',[shard,'1']),('get',[shard])]:
        call('raft-session',op,uuid,*args)
    for op in ('open','close'):
        call('raft-hold',ino,op,'4400',uuid,'1')
    for op in ('ex','un'):
        call('raft-flock',ino,op,'4400',uuid,'1')
        call('raft-fcntl',ino,op,'4400','0','63',uuid,'1')
    call('raft-append',ino,'32',uuid,'1')
    call('expiry-id',ino,expected='expiry-off')
    call('expiry-enable',ino,'1','1','cli-file',time.time_ns()+60_000_000_000,
         expected='expiry-off')
    call('expiry','show','/cli-file',expected='expiry-off')
    call('raft-publish',ino,'0','32',expected='unidentified-report')
    call('raft-link',ino,'1','cli-link')
    call('raft-rename','1','cli-file','1','cli-renamed')
    call('raft-unlink','1','cli-link')
    call('raft-unlink','1','cli-renamed')
    out=call('raft-create','1','cli-dir','040700')
    found=re.search(r'\bino=(\d+)',out);assert found and int(found.group(1)),out
    call('raft-dir',found.group(1),'finish',expected='dir-precondition')
    call('raft-rmdir','1','cli-dir')
    call('raft-session','fence',uuid)
    if kd.fuse_ok():
        mount=kd.Mount(c,1,'mnt')
        with open(a.out/'posix-core.log','w') as log:
            result=subprocess.run([sys.executable,str(root/'tests/posix/posix_suite.py'),
                                   str(mount.path),'--suite','core','--jobs','1'],
                                  env=env,stdout=log,stderr=subprocess.STDOUT,timeout=900)
        (a.out/'posix-core-exit').write_text(str(result.returncode)+'\n')
        if result.returncode:failures.append(dict(command='POSIX core',exit=result.returncode))
    else:
        print('NOTRUN POSIX core: FUSE prerequisites absent',flush=True)
finally:
    if mount:mount.close()
    c.stop_all()
    (a.out/'commands.json').write_text(json.dumps(rows,indent=2)+'\n')
    (a.out/'failures.json').write_text(json.dumps(failures,indent=2)+'\n')
    (a.out/'fixture.txt').write_text(str(work)+'\n')
    (a.out/'identity.txt').write_text(version)
    for path in work.rglob('*.log'):
        if not path.is_symlink():shutil.copyfile(path,a.out/('-'.join(path.relative_to(work).parts)))
    print('kept fixture',work,flush=True)
assert not failures, json.dumps(failures)
print('K44 supplemental CLI surface and POSIX core PASS',flush=True)
