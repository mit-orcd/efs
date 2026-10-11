#!/usr/bin/env python3
"""Read-only final audit of owned AMD daemons and the two allowed mounts."""
import hashlib,json,os,subprocess
from pathlib import Path
p=Path('/data1/efs/amd-round-20261010/restoration-proof.json')
processes=[]
for item in Path('/proc').iterdir():
    if not item.name.isdigit():continue
    try:
        exe=os.readlink(item/'exe')
        if not exe.startswith('/data1/efs/') or Path(exe.removesuffix(' (deleted)')).name not in ('efsd','efs-fuse'):continue
        args=(item/'cmdline').read_bytes().decode().rstrip('\0').split('\0')
        env=dict(v.split('=',1) for v in (item/'environ').read_bytes().decode().split('\0') if '=' in v)
    except (OSError,UnicodeError):continue
    processes.append(dict(pid=int(item.name),exe=exe,args=args,env={key:env.get(key) for key in ('EFS_TRANSPORT','EFS_COMMANDS','EFS_COMMANDS_SERVER','EFS_MD_RAFT_N')}))
assert len(processes)==6,processes
assert all(row['exe'] in ('/data1/efs/src/efsd','/data1/efs/src/efs-fuse') for row in processes),processes
assert sum(Path(row['exe']).name=='efsd' for row in processes)==4,processes
clients=[row for row in processes if Path(row['exe']).name=='efs-fuse']
assert len(clients)==2
for client in clients:
    assert client['env']['EFS_TRANSPORT']=='tcp'
    assert client['env']['EFS_COMMANDS']=='0' and client['env']['EFS_COMMANDS_SERVER']=='0'
mounts=[]
for mount in ('/data1/efs/mnt','/data1/efs/mnt2'):
    assert subprocess.run(['mountpoint','-q',mount]).returncode==0,mount
    assert any(mount in client['args'] for client in clients),mount
    st=os.stat(mount);mounts.append(dict(path=mount,inode=st.st_ino))
version=subprocess.check_output(['/data1/efs/src/efsd','--version'],text=True).strip()
proof=dict(processes=processes,mounts=mounts,installed_version=version,installed_efsd_sha256=hashlib.sha256(Path('/data1/efs/src/efsd').read_bytes()).hexdigest())
p.write_text(json.dumps(proof,indent=2)+'\n')
print('RESTORED: four installed daemons, two TCP/commands-off mounts; no private test daemons remain')
