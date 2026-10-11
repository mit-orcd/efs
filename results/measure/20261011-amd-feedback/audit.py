#!/usr/bin/env python3
"""Independent final process, configuration and two-client namespace audit."""
import hashlib,json,os,subprocess,tempfile
from pathlib import Path
out=Path('/data1/efs/amd-feedback-20261011')
processes=[]
keys=['EFS_TRANSPORT','EFS_COMMANDS','EFS_COMMANDS_SERVER','EFS_MD_RAFT_N','EFS_DATA_GC_ADMISSION','EFS_GC_BACKLOG_IDLE_MS','EFS_RAFT_FLOW','EFS_RAFT_FLOW_EARLY_RETIRE','EFS_RAFT_GROUP_DRAIN','EFS_BATCH_OBS','EFS_RAFT_OBS']
for item in Path('/proc').iterdir():
 if not item.name.isdigit():continue
 try:
  exe=os.readlink(item/'exe')
  if not exe.startswith('/data1/efs/') or Path(exe.removesuffix(' (deleted)')).name not in ('efsd','efs-fuse'):continue
  args=(item/'cmdline').read_bytes().decode().rstrip('\0').split('\0')
  env=dict(v.split('=',1) for v in (item/'environ').read_bytes().decode().split('\0') if '=' in v)
 except (OSError,UnicodeError):continue
 processes.append({'pid':int(item.name),'exe':exe,'args':args,'env':{k:env.get(k) for k in keys}})
assert len(processes)==6,processes
assert all(p['exe'] in ('/data1/efs/src/efsd','/data1/efs/src/efs-fuse') for p in processes)
nodes=[p for p in processes if Path(p['exe']).name=='efsd'];assert len(nodes)==4
assert {p['args'][p['args'].index('--node-id')+1] for p in nodes}=={'1','2','3','4'}
for p in nodes:
 e=p['env'];assert e['EFS_TRANSPORT']=='tcp' and e['EFS_COMMANDS_SERVER']=='0' and e['EFS_MD_RAFT_N']=='4'
 assert e['EFS_DATA_GC_ADMISSION']=='1' and e['EFS_GC_BACKLOG_IDLE_MS']=='20'
 assert all(e[key]=='0' for key in ['EFS_RAFT_FLOW','EFS_RAFT_FLOW_EARLY_RETIRE','EFS_RAFT_GROUP_DRAIN','EFS_BATCH_OBS','EFS_RAFT_OBS'])
 a=p['args'];assert '--direct-io' in a
 nid=a[a.index('--node-id')+1]
 assert a[a.index('--storage')+1]==f'/data1/efs/n{nid}'
 assert int(a[a.index('--port')+1])==17431+int(nid)
 assert Path(f'/data1/efs/logs/efsd-n{nid}.pid').read_text().strip()==str(p['pid'])
 assert Path(f'/data1/efs/logs/efsd-n{nid}.log').is_file()
clients=[p for p in processes if Path(p['exe']).name=='efs-fuse'];assert len(clients)==2
for p in clients:
 assert p['env']['EFS_TRANSPORT']=='tcp' and p['env']['EFS_COMMANDS']=='0' and p['env']['EFS_COMMANDS_SERVER']=='0'
for mount in ['/data1/efs/mnt','/data1/efs/mnt2']:
 assert subprocess.run(['mountpoint','-q',mount]).returncode==0
 assert any(mount in p['args'] for p in clients);os.stat(mount)
folder=Path(tempfile.mkdtemp(prefix='amd-admission-smoke-',dir='/data1/efs/mnt'))
path=folder/'bytes';body=hashlib.sha256(b'AMD admitted smoke').digest()*1024
with path.open('xb') as f:
 assert f.write(body)==len(body);f.flush();os.fsync(f.fileno())
assert path.read_bytes()==body
peer=Path('/data1/efs/mnt2')/folder.name/'bytes';assert peer.read_bytes()==body
path.unlink();folder.rmdir()
proof={'processes':processes,'mounts':['/data1/efs/mnt','/data1/efs/mnt2'],'two_client_acknowledged_smoke_bytes':len(body),
 'installed_version':subprocess.check_output(['/data1/efs/src/efsd','--version'],text=True).strip(),
 'installed_efsd_sha256':hashlib.sha256(Path('/data1/efs/src/efsd').read_bytes()).hexdigest()}
(out/'restoration-proof.json').write_text(json.dumps(proof,indent=2)+'\n')
print('AMD feedback-disabled normal service restored: four direct/TCP admission-enabled daemons, two commands-off clients, canonical identities and peer bytes PASS')
