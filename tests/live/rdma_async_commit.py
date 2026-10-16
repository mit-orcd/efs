#!/usr/bin/env python3
"""Exclusive AMD, private four-node RDMA/direct async-durability regression.

Run: timeout -k 5 180s python3 tests/live/rdma_async_commit.py SOURCE_DIRECTORY
Uses only new /data1/efs/async-commit-* stores and ~/async-commit-* mounts.
Raw logs remain in the printed evidence directory; owned stores are removed
only after normal client teardown. No installed service or Intel mount touched.
The 10s background window exposes COMMIT deterministically; no RPC/test
budget is increased. This is process-crash testing, not a power-cut test.
"""
import errno
import json
import os
from pathlib import Path
import shutil
import subprocess as sp
import sys
import tempfile
import time

src = Path(sys.argv[1]).resolve()
work = Path(tempfile.mkdtemp(prefix='async-commit-', dir='/data1/efs'))
home = Path(tempfile.mkdtemp(prefix='async-commit-', dir=Path.home()))
print(work, flush=True)
env = os.environ.copy()
env.update(EFS_TRANSPORT='rdma', EFS_RDMA_DEV='rxe0', EFS_RDMA_GID_INDEX='1',
           EFS_MD_RAFT_N='4', EFS_METADATA_BALANCE='1', EFS_DIRECT_IO='1',
           EFS_COMMANDS='0', EFS_COMMANDS_SERVER='0',            EFS_EXPIRY='0', EFS_REPAIR_EXPERIMENT='0', EFS_PUT_SYNC_MS='10000',
           EFS_DATA_GC_ADMISSION='1', EFS_GC_BACKLOG_IDLE_MS='20')
for key in ('EFS_RPC_PROF','EFS_READ_PROF','EFS_BATCH_OBS'):
    env.pop(key, None)
base = 28432
nodes = {}
mounts = []
logs = []
checks = []

def command(args, **kw):
    return sp.run([str(x) for x in args], env=env, check=True, timeout=30, **kw)

def start(i):
    log = open(work / ('n%d.log' % i), 'ab'); logs.append(log)
    args = [str(src/'efsd'),'--node-id',str(i),'--addr','127.0.0.1',
            '--port',str(base+i-1),'--storage',str(work/('n%d'%i)),
            '--quota','4G','--direct-io']
    if i != 1: args += ['--join','127.0.0.1:%d'%base]
    p = sp.Popen(args, env=env, stdout=log, stderr=log); nodes[i]=p
    until=time.monotonic()+30
    while time.monotonic()<until:
        assert p.poll() is None, 'daemon exited'
        r=sp.run([str(src/'efs-mgmt'),'raft-status','127.0.0.1:%d'%(base+i-1)],
                 env=env, stdout=sp.DEVNULL, stderr=sp.DEVNULL, timeout=5)
        if r.returncode==0: return
        time.sleep(.25)
    raise AssertionError('daemon readiness timeout')

def mount(name):
    p=home/name; p.mkdir()
    e=env.copy();e['EFS_FUSE_LOG']=str(work/(name+'.log'))
    sp.run([str(src/'scripts/client.sh'),'127.0.0.1:%d'%base,str(p)],
           env=e,check=True,timeout=30,stdout=sp.DEVNULL)
    mounts.append(p)
    return p

def fsync_file(path, expected):
    fd=os.open(path,os.O_RDWR)
    try:
        try: os.fsync(fd)
        except OSError as e:
            assert e.errno==expected, (e.errno,expected)
        else: assert expected==0, 'false durability success'
    finally: os.close(fd)

def stats(label):
    r=command([src/'efs-mgmt','sync-status']+
              ['127.0.0.1:%d'%(base+i) for i in range(4)],capture_output=True)
    (work/(label+'.txt')).write_bytes(r.stdout)
    return r.stdout.decode()

try:
    for i in range(1,5): start(i)
    r=command([src/'efs-mgmt','raft-mkfs','127.0.0.1:%d'%base],capture_output=True)
    assert b'rc=0 ' in r.stdout, r.stdout
    a=mount('a');b=mount('b')
    payload=bytes(range(256))*8192
    (a/'peer.bin').write_bytes(payload) # close publishes, no fsync
    before=stats('before-peer-commit')
    import re
    assert any(int(n)>0 for n in re.findall(r'\((\d+) fragments',before)), before
    fsync_file(b/'peer.bin',0) # this client has never PUT anything
    after=stats('after-peer-commit')
    assert all(int(n)==0 for n in re.findall(r'\((\d+) fragments',after)), after
    assert (b/'peer.bin').read_bytes()==payload
    checks.append('peer fsync commits previously unstable data on all nodes')
    # The writer remains mounted with unresolved PUT accounting. Every clean
    # daemon stop drains its data and persists the same durability verifier.
    for cycle in range(2):
        (a/'clean.bin').write_bytes(payload)
        record=work/'n4'/'.efs-durable-epoch'
        old=record.read_bytes();assert old[:8]==b'EFSDUR02'
        verifier=int.from_bytes(old[40:48],'big');assert verifier
        nodes[4].terminate();nodes[4].wait(timeout=30);start(4)
        assert int.from_bytes(record.read_bytes()[40:48],'big')==verifier
        fsync_file(a/'clean.bin',0)
        fsync_file(b/'clean.bin',0)
        assert (b/'clean.bin').read_bytes()==payload
    checks.append('mounted writer and peer fsync survive two clean daemon restarts')
    (a/'restart.bin').write_bytes(payload)
    stats('before-crash')
    old_verifier=int.from_bytes(record.read_bytes()[40:48],'big')
    nodes[4].kill();nodes[4].wait(timeout=10)
    start(4)
    assert int.from_bytes(record.read_bytes()[40:48],'big')!=old_verifier
    # The old writer must refuse; the server also protects new clients that
    # cannot compare an old verifier. No data corruption is required here.
    fsync_file(a/'restart.bin',errno.EIO)
    fresh=mount('fresh')
    fsync_file(fresh/'restart.bin',errno.EIO)
    fsync_file(fresh/'restart.bin',errno.EIO)
    checks.append('old and fresh clients reject uncertain restarted node')
    # An orderly restart must not launder the previous unclean epoch.
    nodes[4].terminate();nodes[4].wait(timeout=30);start(4)
    fsync_file(fresh/'restart.bin',errno.EIO)
    checks.append('recovery fence survives another orderly restart')
    (work/'result.json').write_text(json.dumps({'status':'PASS','checks':checks},indent=2)+'\n')
    print('PASS: '+ '; '.join(checks),flush=True)
finally:
    clean=True
    for m in reversed(mounts):
        try: command([src/'scripts/client.sh','stop',m],stdout=sp.DEVNULL)
        except Exception as e: print('cleanup retained:',m,e,flush=True);clean=False
    if clean:
        for p in nodes.values():
            if p.poll() is None: p.terminate()
        for p in nodes.values(): p.wait(timeout=30)
        for log in logs: log.close()
        for i in range(1,5): shutil.rmtree(work/('n%d'%i),ignore_errors=True)
        shutil.rmtree(home)
    else:
        print('Fixtures retained; drain clients before stopping nodes:',work,flush=True)
