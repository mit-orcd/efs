#!/usr/bin/env python3
"""Snapshot only Task K product state into external, closed evidence files."""
import hashlib,json,os,subprocess,time
from pathlib import Path
assert os.uname().nodename.split('.')[0] in ('node9900','node9901')
root=Path('/data1/efs/evidence/taskK-orcd-mini-20261027')
def capture(name,args,env=None):
    r=subprocess.run(args,capture_output=True,env=env,timeout=60)
    (root/name).write_bytes(r.stdout+r.stderr)
    assert r.returncode==0,(args,r.returncode)
host=os.uname().nodename.split('.')[0]
source=Path('/data1/efs/src' if host=='node9901' else '/data1/efs/orcd-mini-client/src')
env=dict(os.environ,EFS_TRANSPORT='rdma',EFS_RDMA_DEV='mlx5_0')
capture('final-identity.txt',[str(source/'efsd'),'--version'])
capture('final-status.txt',[str(source/'efs-mgmt'),'status','10.1.99.1:17432'],env)
capture('final-mounts.txt',['findmnt','-rn','-t','fuse.efs-fuse','-o','TARGET,SOURCE,OPTIONS'])
processes=[]
for proc in Path('/proc').glob('[0-9]*'):
    try:
        args=[os.fsdecode(a) for a in (proc/'cmdline').read_bytes().split(b'\0') if a]
        if not args:continue
        if Path(args[0]).name not in ('efsd','efs-fuse'):continue
        # Only deployed product clients/servers, never Task H's private work.
        exe=Path(os.readlink(proc/'exe'))
        if '00e85a299531' not in str(exe):continue
        processes.append(dict(pid=int(proc.name),exe=str(exe),args=args))
    except OSError:pass
(root/'final-processes.json').write_text(json.dumps(processes,indent=2)+'\n')
if host=='node9901':
    assert sum(Path(p['exe']).name=='efsd' for p in processes)==4,processes
    for i in range(1,5):
        capture(f'final-raft-n{i}.txt',[str(source/'efs-mgmt'),'raft-status',f'10.1.99.1:{17431+i}'],env)
        # Snapshot text logs through the current EOF, from this deployment's
        # recorded line boundary; do not hash moving live logs.
        log=Path(f'/data1/efs/logs/efsd-n{i}.log')
        if i<4:
            offset=int(Path(f'/data1/efs/logs/efsd-n{i}.start-offset').read_text().split()[1])
        else:offset=int((root/'n4-log-offset').read_text())
        with log.open('rb') as src,(root/f'current-efsd-n{i}.log').open('wb') as dst:
            for line in range(offset):src.readline()
            remaining=log.stat().st_size-src.tell()
            while remaining:
                block=src.read(min(1<<20,remaining))
                if not block:break
                dst.write(block);remaining-=len(block)
        for suffix in ('.pid','.pid.identity','.start-offset'):
            file=Path(f'/data1/efs/logs/efsd-n{i}{suffix}')
            if file.exists():(root/('current-n'+str(i)+suffix)).write_bytes(file.read_bytes())
    (root/'current-local-client.log').write_bytes(Path('/data1/efs/efs-fuse-mnt.log').read_bytes())
else:
    assert sum(Path(p['exe']).name=='efs-fuse' for p in processes)==1,processes
    (root/'current-remote-client.log').write_bytes(Path('/data1/efs/orcd-mini-client/logs/efs-fuse-orcd-mini-mnt.log').read_bytes())
(root/'capture-time.txt').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())+'\n')
