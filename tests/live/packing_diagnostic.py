#!/usr/bin/env python3
"""Raw output stays outside Git; diagnostic timings are not qualification.
Requires same-user ptrace permission (strace) or perf permission (perf).
Uses only processes launched from --source and the private four-port range.
"""
import argparse,os,pathlib,subprocess,time,signal,json
p=argparse.ArgumentParser(description="Diagnostic only: original-budget seven-client packing with bounded syscall or CPU captures")
p.add_argument("--source",type=pathlib.Path,default=pathlib.Path(__file__).resolve().parents[2])
p.add_argument("--output",type=pathlib.Path,required=True)
p.add_argument("--root",action="append",required=True)
p.add_argument("--port",type=int,default=36910)
p.add_argument("--mount",default="/tmp/efs-pack-seven-a")
p.add_argument("--peer-mount",default="/tmp/efs-pack-seven-b")
p.add_argument("--capture",choices=["strace","perf"],default="strace")
a=p.parse_args()
base=a.output.resolve()
assert not base.exists() or not any(base.iterdir()), "use an empty external output directory"
base.mkdir(parents=True,exist_ok=True)
source=a.source.resolve()
assert len(a.root)==2, 'exactly two storage roots required'
env=dict(os.environ,EFS_PACKED_WRITES='1',EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_REPAIR_EXPERIMENT='0',EFS_POSIX_PHASE_TRACE='1')
for key in ['EFS_PACK_PROF','EFS_RPC_PROF','EFS_READ_PROF','EFS_RAFT_OBS']:
 env.pop(key,None)
log=open(base/'packing-diagnostic.log','wb')
cmd=['python3',str(source/'tests/live/gc_reclamation.py'),'--mode','direct','--transport','tcp','--root',a.root[0],'--root',a.root[1],'--mount',a.mount,'--peer-mount',a.peer_mount,'--port',str(a.port),'--fixture-quota','4G','--mounted-packing-only','--disable-efs-commands','--posix','--packing-clients','7','--posix-jobs','16']
options=a
(base/"capture-config.json").write_text(json.dumps({"command":cmd,"capture":a.capture,"trace_seconds_after_mounts":80},indent=2))
p=subprocess.Popen(cmd,env=env,stdout=log,stderr=log)
tr=[];seen=set();start=time.monotonic();end=None
while p.poll() is None:
 for proc in pathlib.Path('/proc').iterdir():
  if not proc.name.isdigit():continue
  try:args=(proc/'cmdline').read_bytes().split(b'\0');a=[x.decode() for x in args if x]
  except (OSError,UnicodeError):continue
  if not a or a[0] not in [str(source/'efsd'),str(source/'efs-fuse')] or str(options.port) not in ' '.join(a):
   if not a or a[0]!=str(source/'efsd') or not any(str(n) in a for n in range(options.port,options.port+4)):continue
  pid=int(proc.name)
  if pid in seen:continue
  seen.add(pid)
  prefix=base/f'packing-diagnostic-{pid}'
  if options.capture=='strace':
   capture=['strace','-ff','-ttt','-T','-s','16','-e','trace=futex,epoll_wait,poll,ppoll,fsync,fdatasync,pread64,pwrite64,recvfrom,sendto,recvmsg,sendmsg,read,write','-o',str(prefix),'-p',str(pid)]
  else:
   capture=['perf','record','-q','-F','49','-g','--call-graph','dwarf,8192','-o',str(prefix)+'.data','-p',str(pid)]
  t=subprocess.Popen(capture,stdout=subprocess.DEVNULL,stderr=open(str(prefix)+'.attach','wb'))
  tr.append(t);print('attached',pid,a,flush=True)
 if end is None and b'packing extra client 6 mounted PASS' in (base/'packing-diagnostic.log').read_bytes():end=time.monotonic()+80
 if end is not None and time.monotonic()>end:break
 if time.monotonic()-start>180:break
 time.sleep(.5)
for t in tr:
 if t.poll() is None:t.send_signal(signal.SIGINT)
for t in tr:t.wait(timeout=15)
print('trace detached',len(tr),flush=True)
rc=p.wait();(base/'packing-diagnostic.exit').write_text(str(rc)+'\n');print('fixture exit',rc,flush=True)
