#!/usr/bin/env python3
"""Private Intel persistence pause: host-lock progress, owned shutdown, normal restore.

Requires normal/fault builds of identical C source, healthy private nodes and
main mounts. Select a follower-only node; never signal installed production.
An independent remote timer disarms the pause after 10 seconds. Retain logs.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',required=True)
p.add_argument('--fault-source',required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
base='/data1/efs/w100-private/'
runtime=base+'runtime-merged-aa508c820'
for s in [a.source,a.fault_source]:
    if not s.startswith(base) or '..' in s.split('/'):p.error('private source paths required')
a.output.mkdir(parents=True,exist_ok=True)
hosts=['intel1_efs','intel2_efs'];addresses=['10.0.0.207','10.0.0.209']

def call(h,args,**kw):
    return subprocess.run(['ssh','-o','ConnectTimeout=5',h,shlex.join(args)],check=True,text=True,**kw)

def remote(h,code,**kw):return call(h,['python3','-c',code],**kw)

selected=None
for node in [3,1,4,2]:
    h=hosts[(node-1)//2];addr=addresses[(node-1)//2];port=27432+(node%2==0)
    status=call(h,[a.source+'/efs-mgmt','raft-status',f'{addr}:{port}'],capture_output=True,timeout=5).stdout
    rows=[x for x in status.splitlines() if 'hosted=1' in x]
    if rows and all('role=FOLLOWER ' in x for x in rows):
        selected=(node,h,addr,port,status);break
if not selected:raise RuntimeError('no follower-only private node; preserve fixture')
node,h,addr,port,status=selected
pidfile=runtime+f'/n{node}.pid';storage=f'/data{1 if node%2 else 2}/efs/w100-private-merged-aa508c820/n{node}'
gate=runtime+f'/persist-pause-n{node}'
snapshot=remote(h,f'''
import os,json,pathlib
pid=int(pathlib.Path({pidfile!r}).read_text());q=pathlib.Path('/proc')/str(pid)
assert q.stat().st_uid==os.getuid()
args=[os.fsdecode(x) for x in (q/'cmdline').read_bytes().split(b'\\0') if x]
assert os.readlink(q/'exe')=={a.source+'/efsd'!r}
assert args[args.index('--storage')+1]=={storage!r}
env=dict(os.fsdecode(x).split('=',1) for x in (q/'environ').read_bytes().split(b'\\0') if x and b'=' in x)
assert all(env.get(k)=='0' for k in ['EFS_COMMANDS','EFS_COMMANDS_SERVER','EFS_PACKED_WRITES','EFS_EXPIRY'])
assert env.get('EFS_TRANSPORT')=='tcp' and '--direct-io' in args and args[args.index('--storage-engine')+1]=='files'
env={{k:v for k,v in env.items() if k.startswith('EFS_') or k in ('PATH','HOME','LANG')}}
print(json.dumps(dict(pid=pid,args=args,env=env)))
''',capture_output=True).stdout
record=json.loads(snapshot)
# Keep the environment only in memory: it can contain unrelated shell secrets.
args=record['args'];env=record['env'];rows=[dict(node=node,baseline_status=status,original_pid=record['pid'])]
needs_restore=False;work=None

def stop():call(h,['python3',a.source+'/scripts/server_processes.py','stop-file',pidfile,storage],timeout=65)
def off():remote(h,f'from pathlib import Path; Path({gate!r}).write_text("OFF\\n")',timeout=10)
def start(source,fault=False):
    e=env.copy();e.pop('EFS_FAULT_RECV_PERSIST_FILE',None)
    if fault:e['EFS_FAULT_RECV_PERSIST_FILE']=gate
    command=[source+'/efsd']+args[1:]
    result=remote(h,f'''
import pathlib,subprocess,json
with pathlib.Path({runtime+f'/n{node}-persist-qualification.log'!r}).open('ab') as out:
 p=subprocess.Popen({command!r},env={e!r},stdout=out,stderr=subprocess.STDOUT,start_new_session=True)
pathlib.Path({pidfile!r}).write_text(str(p.pid)+'\\n');print(json.dumps(dict(pid=p.pid)))
''',capture_output=True)
    call(h,['python3',source+'/scripts/server_processes.py','record',pidfile,storage])
    return json.loads(result.stdout)['pid']
def ready(source):
    for _ in range(20):
        try:
            text=call(h,[source+'/efs-mgmt','raft-status',f'{addr}:{port}'],capture_output=True,timeout=3).stdout
            if 'hosted=1' in text:return text
        except (subprocess.CalledProcessError,subprocess.TimeoutExpired):pass
        time.sleep(.25)
    raise RuntimeError('private follower did not become ready')

try:
    off();stop();needs_restore=True;fault_pid=start(a.fault_source,True)
    text=ready(a.fault_source)
    assert all('role=FOLLOWER ' in line for line in text.splitlines() if 'hosted=1' in line),text
    # Standalone remote timer survives loss of this controller's connection.
    timer_code=f'import pathlib,time; time.sleep(10); pathlib.Path({gate!r}).write_text("OFF\\n")'
    remote(h,f'import subprocess; subprocess.Popen(["python3","-c",{timer_code!r}],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)')
    remote(h,f'from pathlib import Path; Path({gate+".entered"!r}).unlink(missing_ok=True); Path({gate!r}).write_text("PAUSE\\n")')
    work_log=(a.output/'fsync-workload.log').open('w')
    work=subprocess.Popen(['ssh',hosts[0],shlex.join(['timeout','180','python3',a.source+'/tests/live/fsync_admission.py',runtime+'/mnt','--files','128','--workers','16'])],stdout=work_log,stderr=subprocess.STDOUT)
    for _ in range(80):
        probe=remote(h,f'from pathlib import Path; q=Path({gate+".entered"!r}); print(q.read_text().strip() if q.exists() else "")',capture_output=True).stdout.strip()
        if probe==str(fault_pid):break
        time.sleep(.05)
    else:raise RuntimeError('real receive persistence phase not entered')
    began=time.monotonic()
    probe=call(h,[a.source+'/efs-mgmt','raft-status',f'{addr}:{port}'],capture_output=True,timeout=2).stdout
    elapsed=time.monotonic()-began
    # RAFT_STATUS takes h->mu. The after-check proves it completed while paused.
    mode=remote(h,f'from pathlib import Path; print(Path({gate!r}).read_text().strip())',capture_output=True).stdout.strip()
    assert mode=='PAUSE',('pause ended before lock probe completed',elapsed)
    rows.append(dict(phase_pid=fault_pid,mutex_probe_seconds=elapsed,status_during_pause=probe))
    stop_log=(a.output/'shutdown.log').open('w')
    stopping=subprocess.Popen(['ssh',h,shlex.join(['python3',a.source+'/scripts/server_processes.py','stop-file',pidfile,storage])],stdout=stop_log,stderr=subprocess.STDOUT)
    time.sleep(.5)
    assert stopping.poll() is None,'shutdown returned before retained persistence owner was released'
    remote(h,f'import os; os.kill({fault_pid},0)')
    assert remote(h,f'from pathlib import Path; print(Path({gate!r}).read_text().strip())',capture_output=True).stdout.strip()=='PAUSE'
    rows.append(dict(shutdown_waited_for_owner=True))
    assert stopping.wait(timeout=65)==0;stop_log.close()
    restored_pid=start(a.source);rows.append(dict(restored_pid=restored_pid,restored_status=ready(a.source)));needs_restore=False
    rc=work.wait(timeout=190);work_log.close();assert rc==0,('fsync workload failed',rc)
    print(json.dumps(dict(result='PASS',rows=rows)),flush=True)
    (a.output/'summary.json').write_text(json.dumps(dict(result='PASS',rows=rows),indent=2)+'\n')
finally:
    off()
    if needs_restore:
        stop();start(a.source);ready(a.source)
    if work and work.poll() is None:
        # Existing timeout bounds the workload. No mount or daemon force-kill.
        work.wait(timeout=190)
