#!/usr/bin/env python3
"""Private TCP W88 actual-fragment replay and cold FUSE byte gate.
Requires a clean, empty, host-approved borrowed mount. Never changes installed stores.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

source=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('work',type=Path)
p.add_argument('--mount',type=Path,required=True)
p.add_argument('--mode',choices=('direct','buffered'),default='direct',help='storage I/O mode; default preserves the original direct fixture')
p.add_argument('--cross-group',action='store_true',help='bootstrap and publish on a lane in the other metadata group')
a=p.parse_args();work=a.work.resolve();work.mkdir(parents=True,exist_ok=False)
mount=a.mount.resolve();assert mount.is_dir() and not os.path.ismount(mount) and not any(mount.iterdir())
env=dict(os.environ,EFS_MD_RAFT_N='4',EFS_TRANSPORT='tcp')
if a.cross_group:env['EFS_TEST_CROSS_GROUP']='1'
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
                str(source/'tests/live/publication_replay.c'),str(source/'libefs.a'),
                '-lpthread','-lm','-ldl','-libverbs','-o',str(probe)],check=True)
(work/'manifest.json').write_text(json.dumps({'mode':a.mode,'transport':'tcp','cross_group':a.cross_group,'source':str(source),'probe_source_sha256':hashlib.sha256((source/'tests/live/publication_replay.c').read_bytes()).hexdigest(),'binary_sha256':{name:hashlib.sha256((source/name).read_bytes()).hexdigest() for name in ['efsd','efs-fuse','efs-mgmt','libefs.a']}},indent=2))
procs=[];logs=[];fuse=None
def cold_bytes(label):
    global fuse
    f=(work/f'fuse-{label}.log').open('w');logs.append(f)
    fuse=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{base}',str(mount)],env=env,stdout=f,stderr=f)
    wait(lambda:os.path.ismount(mount))
    expected=(work/'request.bin.expected').read_bytes();actual=(mount/'pending').read_bytes()
    assert actual==expected,(label,len(actual),len(expected),hashlib.sha256(actual).hexdigest(),hashlib.sha256(expected).hexdigest())
    (work/f'bytes-{label}.json').write_text(json.dumps({'bytes':len(actual),'sha256':hashlib.sha256(actual).hexdigest()},indent=2))
    subprocess.run([str(source/'scripts/client.sh'),'stop',str(mount)],env=env,check=True,timeout=100)
    fuse.wait(15);assert fuse.returncode==0;fuse=None
    print('cold exact FUSE bytes '+label+' PASS',flush=True)
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
        if a.mode=='direct':cmd.append('--direct-io')
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
    # Exercise the cold mount before publication. The probe separately
    # establishes server geometry with the exact first immutable fragment PUT.
    warm=(work/'fuse-export-bootstrap.log').open('w');logs.append(warm)
    fuse=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{base}',str(mount)],env=env,stdout=warm,stderr=warm)
    wait(lambda:os.path.ismount(mount))
    assert (mount/'pending').stat().st_size==0
    subprocess.run([str(source/'scripts/client.sh'),'stop',str(mount)],env=env,check=True,timeout=100)
    fuse.wait(15);assert fuse.returncode==0;fuse=None
    # Probe uses a complete dual-group member (node 2) for authoritative routing.
    time.sleep(2)
    subprocess.run([str(probe),str(base),str(source/'efs-mgmt'),'prepare',str(work/'request.bin'),created[0]],env=env,check=True,timeout=90)
    # Prove that the replay has survived actual physical reclamation, rather
    # than merely an old object still present on disk. Generation 100 belongs
    # only to this disposable probe; live replacement generations must remain.
    old_paths=lambda:[str(p) for n in range(1,5) for p in (work/f'n{n}'/'data').rglob('*.100') if p.is_file()]
    wait(lambda:not old_paths())
    live_paths=[str(p) for n in range(1,5) for p in (work/f'n{n}'/'data').rglob('*.201') if p.is_file()]
    assert len(live_paths)==3,live_paths
    (work/'physical-reclamation.json').write_text(json.dumps({'old_generation':100,'remaining_old_fragments':old_paths(),'live_generation':201,'live_fragments':live_paths},indent=2))
    print('old immutable generation physically reclaimed; three live fragments retained PASS',flush=True)
    stop(crash=True)
    start('-restart');time.sleep(3)
    cold_bytes('before-old-reput')
    subprocess.run([str(probe),str(base),str(source/'efs-mgmt'),'finish',str(work/'request.bin'),created[0]],env=env,check=True,timeout=90)
    cold_bytes('after-old-reput-and-retirement')
    stop(crash=True);start('-final');time.sleep(3)
    subprocess.run([str(probe),str(base),str(source/'efs-mgmt'),'verify-retired',str(work/'request.bin'),created[0]],env=env,check=True,timeout=90)
    cold_bytes('after-final-crash')
    stop()
    print('publication replay: real spans, history eviction, old re-PUT and cold FUSE bytes PASS',flush=True)
finally:
    if os.path.ismount(mount):subprocess.run([str(source/'scripts/client.sh'),'stop',str(mount)],env=env,check=True,timeout=100)
    if fuse is not None:
        if fuse.poll() is None:fuse.terminate()
        fuse.wait(15)
    for proc in procs:
        if proc.poll() is None:proc.terminate()
    for proc in procs:
        try:proc.wait(10)
        except subprocess.TimeoutExpired:proc.kill();proc.wait();raise AssertionError('owned fixture needed forced stop')
    for f in logs:f.close()
