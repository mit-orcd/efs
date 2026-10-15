#!/usr/bin/env python3
"""Two-host mounted read-cache smoke. Only creates/removes its unique fixture.
Run locally: python3 THIS.py --writer-host intel1_efs --reader-host amd_efs
Prerequisite: both mounts expose the same export; ssh Python3 available.
"""
import argparse, ctypes, hashlib, json, os, subprocess, sys, time, uuid
from pathlib import Path

def actor(path):
    fd = None
    libc=ctypes.CDLL(None,use_errno=True)
    libc.pread.argtypes=[ctypes.c_int,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_longlong]
    libc.pread.restype=ctypes.c_ssize_t
    for line in sys.stdin:
        q=json.loads(line); op=q['op']
        try:
            if op=='create':
                f=os.open(path,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
                try:
                    data=bytes(range(251))*((16*1024*1024+250)//251)
                    data=data[:16*1024*1024]; pos=0
                    while pos<len(data): pos+=os.write(f,data[pos:])
                    os.fsync(f)
                finally: os.close(f)
            elif op=='patch':
                f=os.open(path,os.O_RDWR)
                try:
                    data=bytes([q['byte']])*q['length']; assert os.pwrite(f,data,q['offset'])==len(data);os.fsync(f)
                finally:os.close(f)
            elif op=='truncate':
                f=os.open(path,os.O_RDWR)
                try:os.ftruncate(f,q['size']);os.fsync(f)
                finally:os.close(f)
            elif op=='read':
                if fd is None:fd=os.open(path,os.O_RDONLY)
                # Force user-memory page offsets, independently of file offset.
                n=q['length']; raw=ctypes.create_string_buffer(n+8192)
                addr=(ctypes.addressof(raw)+4095)&~4095;addr+=q.get('alignment',16)
                got=libc.pread(fd,addr,n,q.get('offset',0))
                if got<0:raise OSError(ctypes.get_errno(),'pread')
                data=ctypes.string_at(addr,got)
                print(json.dumps({'ok':True,'length':got,'sha256':hashlib.sha256(data).hexdigest(),'samples':{str(o):data[o:o+32].hex() for o in [0,131056,131072,131089,131104,1048560]}}),flush=True);continue
            elif op=='unlink':os.unlink(path)
            elif op=='close':
                if fd is not None:os.close(fd);fd=None
            else:raise ValueError(op)
            print(json.dumps({'ok':True}),flush=True)
        except Exception as e:
            print(json.dumps({'ok':False,'error':repr(e)}),flush=True)
    if fd is not None:os.close(fd)

def main(a):
    name='read-reuse-smoke-'+uuid.uuid4().hex
    source=Path(__file__).read_text()
    # Send code as a command argument, reserving stdin for the actor protocol.
    import shlex
    def start(host,mount):
        cmd='python3 -c '+shlex.quote(source)+' --actor '+shlex.quote(mount+'/'+name)
        return subprocess.Popen(['ssh',host,cmd],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    w=start(a.writer_host,a.writer_mount);r=start(a.reader_host,a.reader_mount)
    def call(p,**q):
        p.stdin.write(json.dumps(q)+'\n');p.stdin.flush();out=json.loads(p.stdout.readline());assert out['ok'],out;return out
    content=bytearray((bytes(range(251))*((16*1024*1024+250)//251))[:16*1024*1024])
    checks=0;created=False
    def read(off,n,alignment=16):
        nonlocal checks
        out=call(r,op='read',offset=off,length=n,alignment=alignment)
        expected=bytes(content[off:off+n]);assert out['length']==len(expected) and out['sha256']==hashlib.sha256(expected).hexdigest(),(off,n,out)
        checks+=1
    try:
        call(w,op='create');created=True
        for repeat in range(3):
            for alignment in [0,16,4095]:read(0,1024*1024,alignment)
        for off,n,val in [(131070,9,198),(0,131072,37),(1048560,32,99)]:
            call(w,op='patch',offset=off,length=n,byte=val);content[off:off+n]=bytes([val])*n
            for repeat in range(2):read(0,2*1024*1024)
        if a.close_before_truncate:call(r,op='close')
        if a.settle_seconds:time.sleep(a.settle_seconds)
        call(w,op='truncate',size=131089);del content[131089:]
        if a.refresh_after_truncate:call(r,op='close')
        read(0,2*1024*1024);read(131089,4096)
        if a.close_before_truncate:call(r,op='close')
        if a.settle_seconds:time.sleep(a.settle_seconds)
        call(w,op='truncate',size=2*1024*1024);content.extend(bytes(2*1024*1024-len(content)))
        if a.refresh_after_truncate:call(r,op='close')
        for alignment in [0,16,4095]:read(0,2*1024*1024,alignment)
        # Preserve an open reader across unlink; no stale-data resurrection.
        call(w,op='unlink');created=False;read(0,2*1024*1024)
        print(json.dumps({'verdict':'PASS','checks':checks,'writer':a.writer_host,'reader':a.reader_host,'fixture':name}),flush=True)
    finally:
        call(r,op='close')
        if created:call(w,op='unlink')
        for p in [w,r]:p.stdin.close();assert p.wait(timeout=30)==0

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--actor');p.add_argument('--close-before-truncate',action='store_true');p.add_argument('--settle-seconds',type=float,default=0);p.add_argument('--refresh-after-truncate',action='store_true');p.add_argument('--writer-host',default='intel1_efs');p.add_argument('--reader-host',default='amd_efs');p.add_argument('--writer-mount',default='/data1/efs/mnt');p.add_argument('--reader-mount',default='/data1/efs/intel-mnt');a=p.parse_args()
    actor(a.actor) if a.actor else main(a)
