"""Assert restored service identity/options/mounts after all workloads drain."""
import hashlib,json,os,re,subprocess,time,sys
from pathlib import Path
base=Path(__file__).resolve().parent
gate_results={}
for name in ('default-full10-exit.txt','cohort-exit.txt','native-exit.txt','full100-exit.txt','candidate-full100-exit.txt','kv1000-exit.txt','marker1000-exit.txt','partition-exit.txt','comparison-exit.txt','candidate-kv1000-exit.txt','candidate-marker1000-exit.txt'):
 gate_results[name]=(base/name).read_text().strip() if (base/name).exists() else 'not run'
 if '--restored-only' not in sys.argv:assert gate_results[name]=='0',name
flags=dict(EFS_RAFT_FLOW='1',EFS_RAFT_FLOW_EARLY_RETIRE='0',EFS_RAFT_BATCH_OVERLAP='0',EFS_RAFT_BATCH_NOOP='0',EFS_RAFT_GROUP_DRAIN='0',EFS_BATCH_OBS='0',EFS_COMMANDS_SERVER='0',EFS_RAFT_APPLIED_INTERVAL_MS='10')
servers=[];clients=[];raft={}
for pid in subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split():
 cmd=Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0',b' ').decode()
 env=dict(x.split('=',1) for x in Path(f'/proc/{pid}/environ').read_text().split('\0') if '=' in x)
 actual={k:env.get(k) for k in flags};assert actual==flags,actual
 assert '--direct-io' in cmd and env.get('EFS_TRANSPORT')=='tcp'
 servers.append(dict(pid=int(pid),cmd=cmd,flags=actual,transport=env.get('EFS_TRANSPORT'),exe_sha256=hashlib.sha256(Path(f'/proc/{pid}/exe').read_bytes()).hexdigest()))
 port=re.search(r'--port (\d+)',cmd)[1]
 raft[port]=subprocess.check_output(['/data1/efs/src/efs-mgmt','raft-status','127.0.0.1:'+port],text=True,timeout=10)
 assert 'groups=2' in raft[port]
assert len(servers)==4
assert {v["exe_sha256"] for v in servers}=={hashlib.sha256(Path("/data1/efs/src/efsd").read_bytes()).hexdigest()}
for pid in subprocess.check_output(['pgrep','-x','efs-fuse'],text=True).split():
 env=dict(x.split('=',1) for x in Path(f'/proc/{pid}/environ').read_text().split('\0') if '=' in x)
 assert env.get('EFS_COMMANDS')=='0'
 clients.append(dict(pid=int(pid),commands=env.get('EFS_COMMANDS'),exe_sha256=hashlib.sha256(Path(f'/proc/{pid}/exe').read_bytes()).hexdigest(),cmd=Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0',b' ').decode()))
assert len(clients)==2
assert {c["exe_sha256"] for c in clients}=={hashlib.sha256(Path("/data1/efs/src/efs-fuse").read_bytes()).hexdigest()}
mounts={p:os.path.ismount(p) for p in ('/data1/efs/mnt','/data1/efs/mnt2')};assert all(mounts.values())
processes=subprocess.check_output(['ps','-eo','pid,args'],text=True)
assert not any(re.search(r'(?:python3 .*balanced-load.py|(?:^|/)perf record|(?:^|/)strace -)',x) for x in processes.splitlines())
version=subprocess.check_output(['/data1/efs/src/efsd','--version'],text=True);assert version.startswith('efsd 37ad6a87 ')
record=dict(gate_results=gate_results,restoration_only='--restored-only' in sys.argv,utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),version=version,servers=servers,clients=clients,raft_status=raft,mounts=mounts,binary_sha256={n:hashlib.sha256(Path('/data1/efs/src',n).read_bytes()).hexdigest() for n in ('efsd','efs-fuse')},space=subprocess.check_output(['df','-h','/data1/efs/n1','/data2/efs/n3'],text=True))
(base/'final-service-state.json').write_text(json.dumps(record,indent=2));print(json.dumps(record,indent=2))
