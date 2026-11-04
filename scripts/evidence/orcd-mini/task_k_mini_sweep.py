#!/usr/bin/env python3
"""K44 real-cluster sweep; owns no daemon or mount lifecycle.

Run only after authorized fresh formation and online addition. The named mount
must already use the seed alone and the same product build as --source. Results
are created, never overwritten. Leaves deterministic payloads for the other
host's --verify-only pass. Product and case deadlines are unchanged.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--mount',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--seed',default='10.1.99.1:17432')
p.add_argument('--added',default='10.1.99.1:17435')
p.add_argument('--verify-only',action='store_true')
a=p.parse_args()
a.out.mkdir(parents=True,exist_ok=False)
assert os.path.ismount(a.mount),a.mount
identity=subprocess.check_output([str(a.source/'efsd'),'--version'],text=True)
assert '00e85a299531' in identity,identity
(a.out/'identity.txt').write_text(identity)
rows=[]
fixture=a.mount/'taskK-mini-exact'
payload=bytes(range(256))*4097
expected={'large.bin':payload,'tail.bin':payload[:4099]}
def verify():
    hashes={}
    for name,body in expected.items():
        actual=(fixture/name).read_bytes()
        assert actual==body,(name,len(actual),len(body))
        hashes[name]=dict(bytes=len(body),sha256=hashlib.sha256(actual).hexdigest())
    (a.out/'full-payloads.json').write_text(json.dumps(hashes,indent=2)+'\n')

def call(label,cmd,*args,target=None,refused=False):
    argv=[str(a.source/'efs-mgmt'),cmd,target or a.seed,*map(str,args)]
    begin=time.monotonic()
    result=subprocess.run(argv,capture_output=True,text=True,timeout=60)
    out=result.stdout+result.stderr
    row=dict(label=label,argv=argv,exit=result.returncode,output=out,seconds=time.monotonic()-begin)
    rows.append(row)
    (a.out/'commands.json').write_text(json.dumps(rows,indent=2)+'\n')
    print(json.dumps(row),flush=True)
    if refused:
        assert result.returncode!=0 and 'rc=-15' in out,row
    else:
        assert result.returncode==0,row
        statuses=[int(x) for x in re.findall(r'\bstatus=(\d+)',out)]
        if statuses==[7] and cmd in ('raft-getattr','raft-lookup','raft-lookup-path','raft-readdir','raft-getchunks'):
            hint=re.search(r'\bprimary=(\d+)',out)
            assert hint and int(hint.group(1)) in (2,3,4),row
            # Exactly the returned live hint, not repeated application retries.
            return call(label+' live-hint',cmd,*args,target='10.1.99.1:'+str(17431+int(hint.group(1))))
        assert all(x==0 for x in statuses),row
        assert all(int(x)==0 for x in re.findall(r'\brc=(-?\d+)',out)),row
    return out

try:
    if a.verify_only:
        verify()
    else:
        status=call('retired seed','raft-status')
        assert re.search(r'group 2 .*retired=1',status),status
        added=call('live voters','raft-status',target=a.added)
        assert re.search(r'group 2 .*voters=0x[eE]\b.*joint=0',added),added
        call('seed MKFS','raft-mkfs')
        call('added MKFS','raft-mkfs',target=a.added)
        policy=call('commands get','commands-policy','get')
        version=re.search(r'version=(\d+)',policy).group(1)
        mask=re.search(r'mask=(\d+)',policy).group(1)
        assert mask=='3',policy
        call('commands set','commands-policy','set',mask,version)
        for cmd,args in [('expiry-policy',['cluster','get']),('status',[]),('pack-compact',['0']),
                         ('sync-status',[]),('data-commit',[]),('io-stats',[]),('version',[]),
                         ('gc-status',[]),('expiry-status',[]),('raft-getattr',['1']),
                         ('raft-readdir',['1']),('raft-lookup-path',['/']),('expiry-clock',['get'])]:
            call(cmd,cmd,*args)
        changed=call('same voters','raft-change','2','0xe')
        assert 'converged=1' in changed,changed
        call('retired stepdown','raft-step-down','2',refused=True)
        call('retired resync','raft-resync','2',refused=True)
        call('root setattr','raft-setattr','1','6','0',os.getuid(),os.getgid())
        fixture.mkdir()
        with (fixture/'large.bin').open('wb') as f:
            f.write(payload);f.flush();os.fsync(f.fileno())
        ino=str((fixture/'large.bin').stat().st_ino)
        call('file lookup','raft-lookup',str(fixture.stat().st_ino),'large.bin')
        call('file getattr','raft-getattr',ino)
        call('file chunks','raft-getchunks',ino)
        os.chmod(fixture/'large.bin',0o640)
        os.setxattr(fixture/'large.bin','user.k44',b'exact')
        assert os.getxattr(fixture/'large.bin','user.k44')==b'exact'
        os.removexattr(fixture/'large.bin','user.k44')
        with (fixture/'tail.bin').open('wb') as f:
            f.write(payload);f.flush();os.fsync(f.fileno())
        with (fixture/'tail.bin').open('r+b') as f:
            fcntl.flock(f,fcntl.LOCK_EX);fcntl.lockf(f,fcntl.LOCK_EX,64)
            fcntl.lockf(f,fcntl.LOCK_UN,64);fcntl.flock(f,fcntl.LOCK_UN)
            f.truncate(4099);f.flush();os.fdatasync(f.fileno())
        os.link(fixture/'tail.bin',fixture/'hardlink')
        assert (fixture/'hardlink').read_bytes()==expected['tail.bin']
        (fixture/'hardlink').unlink()
        (fixture/'symlink').symlink_to('tail.bin')
        assert (fixture/'symlink').read_bytes()==expected['tail.bin']
        (fixture/'symlink').unlink()
        (fixture/'empty').mkdir();(fixture/'empty').rmdir()
        (fixture/'large.bin').rename(fixture/'renamed.bin')
        (fixture/'renamed.bin').rename(fixture/'large.bin')
        assert sorted(x.name for x in fixture.iterdir())==sorted(expected)
        assert os.statvfs(fixture).f_blocks>0
        verify()
        final=call('retired final status','raft-status')
        assert not re.search(r'group 2 .*resync=1',final),final
        # Unchanged POSIX core, serial, exactly as private qualification.
        with (a.out/'posix-core.log').open('w') as log:
            result=subprocess.run([sys.executable,str(a.source/'tests/posix/posix_suite.py'),
                str(a.mount),'--suite','core','--jobs','1','--results',str(a.out/'posix-core.tsv')],
                stdout=log,stderr=subprocess.STDOUT,timeout=900)
        (a.out/'posix-core-exit').write_text(str(result.returncode)+'\n')
        assert result.returncode==0,'POSIX core failed; preserved log'
        verify()
    (a.out/'result.json').write_text(json.dumps(dict(verdict='PASS',commands=len(rows),
        exact_files=2,exact_bytes=sum(map(len,expected.values())),verify_only=a.verify_only))+'\n')
except BaseException as exc:
    (a.out/'failure.txt').write_text(repr(exc)+'\n')
    raise
