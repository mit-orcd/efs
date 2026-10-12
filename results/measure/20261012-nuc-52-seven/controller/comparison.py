import os,time,json,signal,subprocess,hashlib
from pathlib import Path
import matrix
b=matrix.home;s=matrix.src;o=b/'results-seven-quiet';o.mkdir(exist_ok=False);matrix.out=o;matrix.ledger=o/'all-gates.jsonl'
manifest=json.loads((b/'inputs.json').read_text());assert manifest['commit']=='52bcfd3570b0a59076c225caad9157b0825954d8'
for n in ('efsd','efs-fuse','efs-mgmt'):
 assert hashlib.sha256((s/n).read_bytes()).hexdigest()==json.loads((b/'results/normal-images.json').read_text())[n]['sha256']
assert not subprocess.run(['pgrep','-f','tiny_files.py|drive-load.py|balanced-load.py|make -j'],stdout=subprocess.DEVNULL).returncode==0,'competing workload'
for m in ('mnt','mnt2'):
 subprocess.run(['/data1/efs/src/scripts/client.sh','stop','/data1/efs/'+m],check=True,timeout=180)
pids=[]
for p in Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  if os.readlink(p/'exe')=='/data1/efs/src/efsd':pids.append(int(p.name))
 except (OSError,PermissionError):pass
for pid in pids:
 fd=os.pidfd_open(pid);signal.pidfd_send_signal(fd,signal.SIGTERM);os.close(fd)
end=time.monotonic()+180
while any(Path('/proc',str(p)).exists() for p in pids):
 assert time.monotonic()<end,'original daemon drain';time.sleep(.5)
work=Path('/data1/efs/nuc-52-group3/seven-quiet');work.mkdir(exist_ok=False)
cmd=['python3','-u',s/'tests/live/gc_reclamation.py','--mode','direct','--transport','tcp','--root',work,'--root',work,'--port','24310','--mount','/data1/efs/mnt','--peer-mount','/data1/efs/mnt2','--mounted-packing-only','--posix','--packing-clients','7','--posix-jobs','16']
rc=matrix.gate('mounted-packing-seven-quiet',cmd,timeout=1800)
(o/'comparison-exit.txt').write_text(str(rc))
(o/'cleanup.json').write_text(json.dumps(dict(mounts={m:os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2')},efsd_processes=subprocess.run(['pgrep','-x','efsd'],capture_output=True,text=True).stdout),indent=2))
