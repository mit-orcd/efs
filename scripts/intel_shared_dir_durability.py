#!/usr/bin/env python3
"""Independent shared-directory durability control; no EFS daemon code.
64 KiB nonzero payload + 4 KiB SHA-256 tail, O_DIRECT, file + directory fsync.
Every worker uses the SAME directory. Checks full records outside timing.
"""
import concurrent.futures
import hashlib
import json
import mmap
import os
from pathlib import Path
import struct
import sys
import tempfile
import threading
import time

parent=Path(sys.argv[1]).resolve(strict=True)
assert parent.is_dir()
body=b''.join(hashlib.sha256(b'intel-shared-dir-20261015'+struct.pack('<I',i)).digest() for i in range(2048))
size=65536+4096
for qd in [4,8,16,16,8,4]:
    root=Path(tempfile.mkdtemp(prefix='shared-q%d-'%qd,dir=parent))
    dfd=os.open(root,os.O_RDONLY|os.O_DIRECTORY)
    os.fsync(dfd)
    barrier=threading.Barrier(qd+1)
    def worker(w):
        buf=mmap.mmap(-1,size);buf[:65536]=body;lat=[]
        barrier.wait()
        for i in range(w,4096,qd):
            start=time.monotonic()
            buf[:16]=struct.pack('<QQ',w,i)
            buf[65536:65568]=hashlib.sha256(buf[:65536]).digest()
            fd=os.open(root/str(i),os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_DIRECT,0o600)
            try:
                if os.write(fd,buf)!=size:raise IOError('short direct write')
                os.fsync(fd)
            finally:os.close(fd)
            os.fsync(dfd);lat.append(time.monotonic()-start)
        buf.close();return lat
    with concurrent.futures.ThreadPoolExecutor(max_workers=qd) as pool:
        futures=[pool.submit(worker,w) for w in range(qd)]
        cpu_start=time.process_time();start=time.monotonic();barrier.wait()
        values=[f.result() for f in futures];seconds=time.monotonic()-start;cpu=time.process_time()-cpu_start
    buf=mmap.mmap(-1,size)
    for i in range(4096):
        fd=os.open(root/str(i),os.O_RDONLY|os.O_DIRECT)
        try:
            if os.readv(fd,[buf])!=size:raise IOError('short direct read')
        finally:os.close(fd)
        assert struct.unpack('<QQ',buf[:16])==(i%qd,i)
        assert buf[16:65536]==body[16:]
        assert buf[65536:65568]==hashlib.sha256(buf[:65536]).digest()
        assert buf[65568:]==bytes(size-65568)
    buf.close()
    for i in range(4096):(root/str(i)).unlink()
    os.fsync(dfd);os.close(dfd);root.rmdir()
    v=sorted(x for row in values for x in row)
    print(json.dumps(dict(qd=qd,objects=4096,seconds=seconds,mib_s=256/seconds,
        cpu_seconds=cpu,mean_ms=sum(v)/len(v)*1000,p95_ms=v[int(.95*(len(v)-1))]*1000,
        verified=4096,cleanup=True,time=time.time())),flush=True)
