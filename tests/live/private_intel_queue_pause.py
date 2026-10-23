#!/usr/bin/env python3
"""Private Intel queue-guard qualification: pause one owned daemon, exact fsync/peer reads.

Run on the Mac with intel1_efs/intel2_efs SSH aliases. Only the named private
runtime/node3 is signalled, through a verified pidfd. A separate remote process
resumes it after 10 seconds, including exception paths. Files stay for cold
verification; --verify ROOT checks them again without injecting a failure.
"""
import argparse, concurrent.futures, json, shlex, subprocess, uuid
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',required=True)
p.add_argument('--runtime',default='/data1/efs/w100-private/runtime-merged-aa508c820')
p.add_argument('--verify')
a=p.parse_args()
base='/data1/efs/w100-private/'
for v in [a.source,a.runtime]:
    if not v.startswith(base) or '..' in v.split('/'):p.error('owned private paths required')
run=a.runtime.removeprefix(base+'runtime-')
if not a.runtime.startswith(base+'runtime-') or not run or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-' for c in run):
    p.error('owned runtime label required')
storage='/data1/efs/w100-private-'+run+'/n3'
hosts=['intel1_efs','intel2_efs'];mnt=a.runtime+'/mnt'
root=a.verify or mnt+'/queue-pause-'+uuid.uuid4().hex[:12]
if not root.startswith(mnt+'/queue-pause-') or '..' in root.split('/'):p.error('owned fixture root required')
def remote(h,code,timeout=90):
    return subprocess.run(['ssh','-o','ConnectTimeout=5',h,'python3 -'],input=code,text=True,capture_output=True,timeout=timeout,check=True)
common='''import os,pathlib,hashlib,time,json,concurrent.futures
root=pathlib.Path(ROOT)
def payload(h,i):return hashlib.sha256((h+str(i)).encode()).digest()*32
'''.replace('ROOT',repr(root))
rows=[]
if not a.verify:
    remote(hosts[0],common+'root.mkdir(exist_ok=False)\n')
    # The pause timer owns its pidfd; no unrelated process or production PID
    # can be signalled, even if the daemon exits or the controller is cancelled.
    pause=f'''import os,pathlib,signal,time,json
r=pathlib.Path({a.runtime!r});pid=int((r/'n3.pid').read_text());fd=os.pidfd_open(pid)
try:
 q=pathlib.Path('/proc')/str(pid)
 assert q.stat().st_uid==os.getuid()
 assert os.readlink(q/'exe')=={a.source+'/efsd'!r}
 args=(q/'cmdline').read_bytes().split(b'\\0')
 assert b'--port' in args and args[args.index(b'--port')+1]==b'27432'  # member 3: intel2:27432 (D39 endpoint)
 assert args[args.index(b'--storage')+1]=={storage.encode()!r}
 signal.pidfd_send_signal(fd,signal.SIGSTOP)
 try:
  print(json.dumps(dict(paused=pid,seconds=10)),flush=True);time.sleep(10)
 finally:signal.pidfd_send_signal(fd,signal.SIGCONT)
finally:os.close(fd)
'''
    pause_process=subprocess.Popen(['ssh','-o','ConnectTimeout=5',hosts[1],'python3 -c '+shlex.quote(pause)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        ready=pause_process.stdout.readline();assert ready, pause_process.stderr.read()
        rows.append(json.loads(ready))
        def write(h):
            code=common+f'''
h={h!r};d=root/h;d.mkdir();started=time.monotonic()
def one(i):
 t=time.monotonic();path=d/str(i);data=payload(h,i)
 fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
 try:
  assert os.write(fd,data)==len(data);os.fsync(fd)
 finally:os.close(fd)
 elapsed=time.monotonic()-t;assert elapsed<30,(i,elapsed);return elapsed
with concurrent.futures.ThreadPoolExecutor(8) as ex:times=list(ex.map(one,range(64)))
print(json.dumps(dict(host=h,files=64,seconds=time.monotonic()-started,max_case_s=max(times))))
'''
            return json.loads(remote(h,code).stdout)
        with concurrent.futures.ThreadPoolExecutor(2) as ex:rows.extend(ex.map(write,hosts))
    finally:
        # Do not terminate the remote safety timer; it must restore SIGCONT.
        out,err=pause_process.communicate(timeout=20)
        if pause_process.returncode:raise RuntimeError(('pause timer failed',out,err))
def verify(h):
    code=common+'''
for writer in ['intel1_efs','intel2_efs']:
 for i in range(64):
  data=payload(writer,i);f=root/writer/str(i)
  assert f.stat().st_size==len(data) and f.read_bytes()==data,(writer,i)
print(json.dumps(dict(verified=128,bytes=128*1024)))
'''
    return dict(host=h,**json.loads(remote(h,code).stdout))
with concurrent.futures.ThreadPoolExecutor(2) as ex:rows.extend(ex.map(verify,hosts))
print(json.dumps(dict(result='PASS',fixture=root,rows=rows)),flush=True)
