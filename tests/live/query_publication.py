#!/usr/bin/env python3
"""Private TCP fixture: background query publication/startup/shutdown smoke.

This is NOT .find/.stats RPC, permissions, fault or large-tree acceptance.
Keeps raw stores/logs in a caller-provided NEW directory. Never touches a
running cluster or mounts; terminates only the subprocesses it starts.
"""
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time

source=Path(__file__).resolve().parents[2]
work=Path(sys.argv[1]).resolve()
work.mkdir(parents=True,exist_ok=False)
logs=[];procs=[]
env=dict(os.environ,EFS_MD_RAFT_N='4',EFS_TRANSPORT='tcp')
# Reserve consecutive candidates while checking; no unrelated process is killed.
for base in range(24800,25000,4):
    held=[]
    try:
        for port in range(base,base+4):
            sock=socket.socket();held.append(sock);sock.bind(('127.0.0.1',port))
        break
    except OSError:
        continue
    finally:
        for sock in held:sock.close()
else:
    raise RuntimeError('no free fixture port range')
print(f'fixture={work} ports={base}-{base+3}',flush=True)
def run(*args):
    return subprocess.run([str(source/'efs-mgmt'),*map(str,args)],env=env,text=True,
                          stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=8)
def wait_for(fn,seconds=25):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        assert all(p.poll() is None for p in procs),'fixture daemon exited'
        if fn():return
        time.sleep(.1)
    raise AssertionError('timed out waiting for fixture condition')
def published(node,files):
    text=(work/f'n{node}.log').read_text()
    return any(re.search(rf'root_rc=0 files={files} dirs=0 logical=0(?:\s|$)',line)
               for line in text.splitlines() if 'query-publish:' in line)
try:
    for node in range(1,5):
        log=(work/f'n{node}.log').open('w');logs.append(log)
        args=[str(source/'efsd'),'--node-id',str(node),'--addr','127.0.0.1',
              '--port',str(base+node-1),'--storage',str(work/f'n{node}'),
              '--quota','1G','--writers','0','--no-direct-io']
        if node>1:args+=['--join',f'127.0.0.1:{base}']
        procs.append(subprocess.Popen(args,env=env,stdout=log,stderr=log))
        wait_for(lambda: 'listening' in (work/f'n{node}.log').read_text(),10)
    def mkfs():
        for node in range(4):
            result=run('raft-mkfs',f'127.0.0.1:{base+node}')
            print(result.stdout.strip(),flush=True)
            if result.returncode==0:return True
        return False
    wait_for(mkfs)
    wait_for(lambda:published(2,0) and published(3,0))
    print('initial complete-group catalogs PASS',flush=True)
    # Parent root=1; actual Raft mutations, no test-only catalog injection.
    for index in range(4):
        def create():
            for node in range(4):
                result=run('raft-create',f'127.0.0.1:{base+node}',1,f'file-{index}')
                print(result.stdout.strip(),flush=True)
                if result.returncode==0:return True
            return False
        wait_for(create)
    wait_for(lambda:published(2,4) and published(3,4))
    assert not any('query-publish:' in (work/f'n{node}.log').read_text() for node in (1,4)), 'partial hosts published namespace'
    print('quiet metadata refresh and partial-host refusal PASS',flush=True)
    for proc in procs:proc.terminate()
    for proc in procs:
        proc.wait(8);assert proc.returncode==0,'first fixture shutdown'
    procs.clear()
    for node in range(1,5):
        log=(work/f'n{node}-restart.log').open('w');logs.append(log)
        args=[str(source/'efsd'),'--node-id',str(node),'--addr','127.0.0.1',
              '--port',str(base+node-1),'--storage',str(work/f'n{node}'),
              '--quota','1G','--writers','0','--no-direct-io']
        if node>1:args+=['--join',f'127.0.0.1:{base}']
        procs.append(subprocess.Popen(args,env=env,stdout=log,stderr=log))
        wait_for(lambda: 'listening' in (work/f'n{node}-restart.log').read_text(),10)
    def rebuilt(node):
        return any('generation=1 ' in line and 'rebuilt=4096 ' in line and 'root_rc=0 files=4 dirs=0 logical=0' in line
                   for line in (work/f'n{node}-restart.log').read_text().splitlines() if 'query-publish:' in line)
    wait_for(lambda:rebuilt(2) and rebuilt(3))
    print('restart rebuild from persisted metadata PASS',flush=True)
finally:
    for proc in procs:
        if proc.poll() is None:proc.terminate()
    for proc in procs:
        try:proc.wait(8)
        except subprocess.TimeoutExpired:
            proc.kill();proc.wait()
            raise AssertionError('fixture shutdown timeout; killed owned process')
        assert proc.returncode==0,f'fixture daemon exit {proc.returncode}'
    for log in logs:log.close()
print('query_publication: PASS (smoke only)',flush=True)
