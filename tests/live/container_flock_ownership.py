#!/usr/bin/env python3
"""Mounted flock ownership regression; use only an explicitly isolated export."""
import argparse, fcntl, json, os, selectors, shutil, subprocess, sys, tempfile, time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--mount',type=Path,required=True)
a=p.parse_args()
root=Path(tempfile.mkdtemp(prefix='container-flock-ownership-',dir=a.mount))
file=root/'f';fd=os.open(file,os.O_CREAT|os.O_RDWR,0o600)
rows=[];waiter=None;other=None
probe='''import fcntl,os,sys
f=os.open(sys.argv[1],os.O_RDWR)
try:
 try:fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB)
 except BlockingIOError:print("blocked")
 else:print("acquired");fcntl.flock(f,fcntl.LOCK_UN)
finally:os.close(f)
'''
def check(name,expected,preserve=()):
    r=subprocess.run([sys.executable,'-c',probe,str(file)],pass_fds=preserve,capture_output=True,text=True,timeout=5)
    assert r.returncode==0 and r.stdout.strip()==expected,(name,r.returncode,r.stdout,r.stderr)
    rows.append({'case':name,'status':'PASS'})
try:
    fcntl.flock(fd,fcntl.LOCK_EX)
    check('closing inherited copy preserves parent lock','blocked')
    check('preserved inherited copy control','blocked',(fd,))
    other=os.open(file,os.O_RDWR)
    os.close(other);other=None
    check('unlocked description close preserves locked description','blocked',(fd,))
    fcntl.flock(fd,fcntl.LOCK_UN)
    check('explicit unlock permits peer','acquired',(fd,))
    fcntl.flock(fd,fcntl.LOCK_EX)
    waiter=subprocess.Popen([sys.executable,'-c',
        'import os,fcntl,sys;f=os.open(sys.argv[1],os.O_RDWR);print("attempt",flush=True);fcntl.flock(f,fcntl.LOCK_EX);print("acquired",flush=True);fcntl.flock(f,fcntl.LOCK_UN);os.close(f)',str(file)],
        pass_fds=(fd,),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    with selectors.DefaultSelector() as selector:
        selector.register(waiter.stdout,selectors.EVENT_READ)
        assert selector.select(5),'blocking child did not reach lock attempt'
        assert waiter.stdout.readline().strip()=='attempt'
    time.sleep(.25)
    assert waiter.poll() is None,'blocking flock returned before explicit unlock'
    fcntl.flock(fd,fcntl.LOCK_UN)
    out,err=waiter.communicate(timeout=5)
    assert waiter.returncode==0 and out.strip()=='acquired',(waiter.returncode,out,err)
    rows.append({'case':'blocking flock waits and acquires after unlock','status':'PASS'})
    other=os.open(file,os.O_RDWR)
    fcntl.flock(fd,fcntl.LOCK_EX)
    os.close(fd);fd=None
    check('locked description release while another open remains','acquired',(other,))
finally:
    if waiter and waiter.poll() is None:waiter.kill();waiter.wait()
    if fd is not None:os.close(fd)
    if other is not None:os.close(other)
    shutil.rmtree(root)
print(json.dumps(rows))
