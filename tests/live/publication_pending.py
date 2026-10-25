#!/usr/bin/env python3
"""Four private TCP daemons: lost admission ACK and cold pending recovery.
Never mounts, stops installed service, or changes installed stores. Keeps evidence.
"""
import argparse
import os
from pathlib import Path
import re
import socket
import subprocess
import time

source=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('work',type=Path)
p.add_argument('--apply-pending',action='store_true')
a=p.parse_args();work=a.work.resolve();work.mkdir(parents=True,exist_ok=False)
env=dict(os.environ,EFS_TRANSPORT='tcp')
gate=work/'park-publication-apply'
if a.apply_pending:env['EFS_FAULT_PUBLICATION_APPLY_FILE']=str(gate)
for base in range(25000,25200,4):
    sockets=[]
    try:
        for port in range(base,base+4):
            s=socket.socket();sockets.append(s);s.bind(('127.0.0.1',port))
        break
    except OSError:
        continue
    finally:
        for s in sockets:s.close()
else:raise RuntimeError('no free fixture ports')
probe=work/'probe'
subprocess.run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),
                '-I'+str(source/'src/client'),str(source/'tests/live/publication_pending.c'),str(source/'libefs.a'),
                '-lpthread','-lm','-ldl','-libverbs','-o',str(probe)],check=True)
procs=[];logs=[]
def wait(fn):
    end=time.monotonic()+40
    while time.monotonic()<end:
        assert all(p.poll() is None for p in procs),'owned daemon exited'
        if fn():return
        time.sleep(.1)
    raise AssertionError('fixture timeout')
def mgmt(*args):
    return subprocess.run([str(source/'efs-mgmt'),*map(str,args)],env=env,text=True,
                          stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=15)
def start(suffix):
    for n in range(4):
        f=(work/f'n{n+1}{suffix}.log').open('w');logs.append(f)
        cmd=[str(source/'efsd'),'--node-id',str(n+1),'--addr','127.0.0.1','--port',str(base+n),
             '--storage',str(work/f'n{n+1}'),'--quota','1G','--writers','0']
        if n:cmd+=['--join',f'127.0.0.1:{base}']
        procs.append(subprocess.Popen(cmd,env=env,stdout=f,stderr=f))
        wait(lambda:'listening' in (work/f'n{n+1}{suffix}.log').read_text())
def stop(crash=False):
    for proc in procs:
        if crash:proc.kill()
        else:proc.terminate()
    for proc in procs:
        proc.wait(10)
        assert proc.returncode==(-9 if crash else 0),proc.returncode
    procs.clear()
try:
    start('')
    def format_cluster():
        # W153: formation commits the members joined at that moment.
        if 'Cluster nodes (4)' not in mgmt('status',f'127.0.0.1:{base}').stdout:return False
        for n in range(4):
            result=mgmt('raft-mkfs',f'127.0.0.1:{base+n}')
            if not result.returncode:return True
        return False
    wait(format_cluster)
    created=[]
    def create():
        for n in range(4):
            r=mgmt('raft-create',f'127.0.0.1:{base+n}',1,'pending')
            match=re.search(r'ino=(\d+)',r.stdout)
            if not r.returncode and match:created.append(match[1]);return True
        return False
    wait(create)
    # Probe uses a complete dual-group member (node 2) for authoritative routing.
    time.sleep(2)
    subprocess.run([str(probe),str(base),str(source/'efs-mgmt'),'prepare',str(work/'request.bin'),created[0]],env=env,check=True,timeout=90)
    stop(crash=True)
    start('-restart');time.sleep(3)
    if a.apply_pending:gate.touch()
    subprocess.run([str(probe),str(base),str(source/'efs-mgmt'),'park' if a.apply_pending else 'finish',str(work/'request.bin'),created[0]],env=env,check=True,timeout=90)
    if a.apply_pending:
        stop(crash=True);gate.unlink();start('-mapping-restart');time.sleep(3)
        subprocess.run([str(probe),str(base),str(source/'efs-mgmt'),'resolve',str(work/'request.bin'),created[0]],env=env,check=True,timeout=90)
    stop()
    print(f'publication pending: direct/TCP lost ACK, four-daemon crash and cold recovery PASS',flush=True)
finally:
    for proc in procs:
        if proc.poll() is None:proc.terminate()
    for proc in procs:
        try:proc.wait(10)
        except subprocess.TimeoutExpired:proc.kill();proc.wait();raise AssertionError('owned fixture needed forced stop')
    for f in logs:f.close()
