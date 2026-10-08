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
logs=[];procs=[];fuse_procs=[];mounts=[]
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
    if os.environ.get('EFS_QUERY_RPC')=='1':
        from query_rpc import request,query
        reply,text=query(base+1,request())
        assert reply.status==0,(reply.status,text)
        assert b'tree_files=4\n' in text and reply.checked_dirs==1,text
        generation=reply.generation;applied=list(reply.applied)
        reply,text=query(base,request())
        assert reply.status==-15 and reply.primary_id in (2,3),(reply.status,reply.primary_id)
        for _ in range(12):
            reply,text=query(base+1,request(operation=1,match=1,term=b'file-'))
            assert reply.status==0 and set(text.splitlines())=={f'file-{i}'.encode() for i in range(4)},(reply.status,text)
            assert reply.generation==generation and list(reply.applied)==applied,'query rebuilt catalog'
        for fields,status in [({'version':2},-5),({'ngroups':129},-5),({'max_output':1},-22),
                              ({'operation':1,'match':1,'term':b'file-','visit_budget':1},-13),
                              ({'operation':1,'term':b'bad*term'},-5),({'root':999999},-3)]:
            reply,text=query(base+1,request(**fields));assert reply.status==status,(fields,reply.status,status)
        reply,text=query(base+1,request(),raw=b'bad')
        assert reply.status==-5,reply.status
        print('v1 RPC stats/find, unchanged publication, redirects and explicit errors PASS',flush=True)

    expected_root='root_rc=0 files=4 dirs=0 logical=0'
    if os.environ.get('EFS_QUERY_FUSE')=='1':
        for index in (1,2):
            mount=work/f'mnt{index}';mount.mkdir();mounts.append(mount)
            log=(work/f'fuse{index}.log').open('w');logs.append(log)
            fuse_procs.append(subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{base}',
                                                'w92-private',str(mount)],env=env,stdout=log,stderr=log))
            wait_for(lambda:os.path.ismount(mount),20)
        env['EFS_QUERY_TEST_PORT']=str(base+1)
        with (work/'virtual-gates.log').open('w') as output:
            result=subprocess.run([sys.executable,str(source/'tests/live/virtual_interfaces.py'),
                                   *map(str,mounts)],env=env,stdout=output,stderr=subprocess.STDOUT,timeout=240)
        assert result.returncode==0,(work/'virtual-gates.log').read_text()
        print('two-mount virtual interface gates PASS',flush=True)
        if os.environ.get('EFS_QUERY_POSIX')=='1':
            suites=[('posix',[str(source/'tests/posix/posix_suite.py'),str(mounts[0]),'--results',str(work/'posix.tsv')]),
                    ('peer',[str(source/'tests/posix/posix_2client.py'),'--local',*map(str,mounts),'--results',str(work/'peer.tsv')])]
            for label,args in suites:
                with (work/f'{label}.log').open('w') as output:
                    result=subprocess.run([sys.executable,*args],env=env,stdout=output,stderr=subprocess.STDOUT,timeout=360)
                assert result.returncode==0,(work/f'{label}.log').read_text()
                print(label+' full suite PASS',flush=True)

        # Independent syscall oracle after workloads close; this test-only
        # walk is never part of a virtual query or server publication path.
        total_files=total_dirs=total_bytes=0
        for directory,dirs,files in os.walk(mounts[0]):
            dirs[:]=[name for name in dirs if name not in ('.find','.stats')]
            files=[name for name in files if name not in ('.find','.stats')]
            total_dirs+=len(dirs);total_files+=len(files)
            total_bytes+=sum(os.lstat(Path(directory)/name).st_size for name in files)
        expected_root=f'root_rc=0 files={total_files} dirs={total_dirs} logical={total_bytes}'
        print('restart oracle '+expected_root,flush=True)
        for mount in mounts:subprocess.run(['fusermount3','-u',str(mount)],check=True,timeout=10)
        for proc in fuse_procs:proc.wait(10);assert proc.returncode==0,proc.returncode
        fuse_procs.clear();mounts.clear()
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
        return any('generation=1 ' in line and 'rebuilt=4096 ' in line and expected_root in line
                   for line in (work/f'n{node}-restart.log').read_text().splitlines() if 'query-publish:' in line)
    wait_for(lambda:rebuilt(2) and rebuilt(3))
    print('restart rebuild from persisted metadata PASS',flush=True)
finally:
    for mount in mounts:
        subprocess.run(['fusermount3','-u',str(mount)],timeout=10,check=False)
    for proc in fuse_procs:
        if proc.poll() is None:proc.terminate()
        try:proc.wait(10)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
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
