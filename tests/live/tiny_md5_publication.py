#!/usr/bin/env python3
"""Private Intel: seven fsync/close/rename writers plus seven peer MD5 crawlers.

Each ready name appears only after data fsync and close succeed. Crawlers check
immutable ready files, so incomplete creates cannot produce a false checksum
failure. No app retries. Retain every fixture, including failures, for inspection
and --role verify after a cold private restart. Controller/worker markers live
on local XFS, outside the EFS namespace. Existing per-RPC deadlines are unchanged.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time
import uuid

HOSTS=['intel1_efs','intel2_efs']
BASE='/data1/efs/w100-private/'
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--role',choices=['run','worker','verify'],default='run')
p.add_argument('--source')
p.add_argument('--mount',type=Path,default=Path(BASE+'runtime-merged-aa508c820/mnt'))
p.add_argument('--relative')
p.add_argument('--files',type=int,default=2000)
p.add_argument('--host-index',type=int,choices=[0,1],default=0)
p.add_argument('--control',type=Path)
p.add_argument('--output',type=Path)
a=p.parse_args()
if not str(a.mount).startswith(BASE+'runtime-') or a.mount.name!='mnt':p.error('private Intel main mount required')
if not 1<=a.files<=20000:p.error('files must be 1..20000')

def payload(writer,i):return hashlib.sha256(f'{writer}/{i}'.encode()).digest()*4

def check_file(root,writer,i):
    path=root/f'w{writer}'/f'{i}.ready'
    data=path.read_bytes();want=payload(writer,i)
    if data!=want or hashlib.md5(data).digest()!=hashlib.md5(want).digest():
        raise AssertionError(('payload/EOF',str(path),len(data)))


def worker(root,control):
    assert os.path.ismount(a.mount) and control.is_dir()
    writers=list(range(4) if a.host_index==0 else range(4,7))
    crawlers=list(range(4,7) if a.host_index==0 else range(4))
    start=time.monotonic()
    def write(w):
        phase='create';i=0
        try:
            for i in range(a.files):
                part=root/f'w{w}'/f'{i}.part';ready=part.with_suffix('.ready')
                phase='create'
                with part.open('xb',buffering=0) as f:
                    phase='write';data=payload(w,i);assert f.write(data)==len(data)
                    phase='fsync';os.fsync(f.fileno());phase='close'
                phase='rename';part.rename(ready)
            return dict(role='write',writer=w,result='PASS',files=a.files)
        except Exception as e:
            return dict(role='write',writer=w,result='FAIL',file=i,phase=phase,error=repr(e),context=repr(e.__context__))
    def crawl(w):
        checked=0;passes=0
        try:
            while not (control/'stop').exists():
                for path in (root/f'w{w}').iterdir():
                    if path.suffix!='.ready':continue
                    i=int(path.stem);assert 0<=i<a.files
                    check_file(root,w,i);checked+=1
                passes+=1;time.sleep(.05)
            for i in range(a.files):check_file(root,w,i);checked+=1
            return dict(role='crawl',writer=w,result='PASS',checks=checked,passes=passes)
        except Exception as e:
            return dict(role='crawl',writer=w,result='FAIL',checks=checked,passes=passes,error=repr(e))
    with concurrent.futures.ThreadPoolExecutor(7) as ex:
        wf=[ex.submit(write,w) for w in writers];cf=[ex.submit(crawl,w) for w in crawlers]
        rows=[f.result() for f in wf]
        (control/'writers-done').write_text(json.dumps(rows))
        rows.extend(f.result() for f in cf)
    good=all(r['result']=='PASS' for r in rows)
    print(json.dumps(dict(result='PASS' if good else 'FAIL',fixture=str(root),files_per_writer=a.files,seconds=time.monotonic()-start,rows=rows)),flush=True)
    return 0 if good else 1


def remote(h,args,**kwargs):
    return subprocess.run(['ssh','-o','ConnectTimeout=5',h,shlex.join(args)],check=True,**kwargs)


def run():
    if not a.source or not a.source.startswith(BASE) or '..' in a.source.split('/'):p.error('private frozen source required')
    if not a.output:p.error('--output required')
    a.output.mkdir(parents=True,exist_ok=True)
    relative='receive-load-'+uuid.uuid4().hex[:12];root=a.mount/relative
    control=BASE+'results/'+relative
    init=f'import pathlib; r=pathlib.Path({str(root)!r}); r.mkdir(exist_ok=False); [ (r/f"w{{i}}").mkdir() for i in range(7)]'
    remote(HOSTS[0],['python3','-c',init])
    procs=[];logs=[];started=time.monotonic()
    try:
        for i,h in enumerate(HOSTS):
            remote(h,['mkdir','-p',control])
            remote(h,['python3','-c',f'from pathlib import Path; p=Path({control!r}); assert not (p/"stop").exists() and not (p/"writers-done").exists()'])
            subprocess.run(['scp',str(Path(__file__).resolve()),h+':'+control+'/worker.py'],check=True)
            logfile=(a.output/(h+'-tiny-md5.log')).open('w');logs.append(logfile)
            args=['timeout','--kill-after=5s','1800s','python3',control+'/worker.py','--role','worker','--mount',str(a.mount),'--relative',relative,'--files',str(a.files),'--host-index',str(i),'--control',control]
            procs.append(subprocess.Popen(['ssh',h,shlex.join(args)],stdout=logfile,stderr=subprocess.STDOUT))
        # Control-plane polling only; no retry of an application operation.
        deadline=started+1800
        while time.monotonic()<deadline:
            if any(proc.poll() is not None for proc in procs):break
            ready=[]
            for h in HOSTS:
                r=subprocess.run(['ssh',h,shlex.join(['test','-e',control+'/writers-done'])],timeout=10)
                ready.append(r.returncode==0)
            if all(ready):break
            time.sleep(.5)
    finally:
        # Let every crawler do its final exact pass, including missing-file
        # failures when a writer failed. Keep all data; never discard a mount.
        for h in HOSTS:
            remote(h,['python3','-c',f'from pathlib import Path; Path({control+"/stop"!r}).touch()'])
        exits=[]
        for proc in procs:exits.append(proc.wait(timeout=max(10,started+1810-time.monotonic())))
        for f in logs:f.close()
    record=dict(result='PASS' if len(exits)==2 and not any(exits) else 'FAIL',source=a.source,fixture=str(root),files_per_writer=a.files,writers=7,crawlers=7,seconds=time.monotonic()-started,exits=exits,worker_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),remote_control=control)
    (a.output/'tiny-md5-summary.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record),flush=True)
    return int(record['result']!='PASS')

if __name__=='__main__':
    if a.role=='run':raise SystemExit(run())
    if not a.relative or not a.relative.startswith('receive-load-') or '/' in a.relative or '..' in a.relative:p.error('owned fixture relative name required')
    root=a.mount/a.relative
    if a.role=='verify':
        assert os.path.ismount(a.mount)
        for w in range(7):
            for i in range(a.files):check_file(root,w,i)
        print(json.dumps(dict(result='PASS',fixture=str(root),files=7*a.files)),flush=True)
    else:
        if not a.control or '..' in a.control.parts or not str(a.control).startswith(BASE+'results/'):p.error('private local control path required')
        raise SystemExit(worker(root,a.control))
