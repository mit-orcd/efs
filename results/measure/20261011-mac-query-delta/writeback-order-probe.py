import os, time, tempfile, statistics, json, shutil, ctypes
libc=ctypes.CDLL(None,use_errno=True)
writeback=libc.sync_file_range
writeback.argtypes=[ctypes.c_int,ctypes.c_longlong,ctypes.c_longlong,ctypes.c_uint]
writeback.restype=ctypes.c_int
base='/data1/mac-query-sync-probe-20261011'
os.makedirs(base,exist_ok=True)
# Fixed small files matching cache raw/index writers. Every writing descriptor
# is checked before close; only write/flush ordering differs.
for sample in range(3):
    for batch in ([1,16] if sample%2==0 else [16,1]):
        root=tempfile.mkdtemp(prefix=f's{sample}-b{batch}-',dir=base)
        started=time.monotonic_ns(); times=[]
        try:
            for first in range(0,128,batch):
                descriptors=[]
                try:
                    for index in range(first,first+batch):
                        fd=os.open(f'{root}/{index}.r',os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
                        descriptors.append(fd)
                        remaining=memoryview(b'x'*4096)
                        while remaining:
                            wrote=os.write(fd,remaining)
                            if wrote<=0:raise OSError('zero-length write')
                            remaining=remaining[wrote:]
                    if batch==16:
                        for fd in descriptors:
                            if writeback(fd,0,0,2):raise OSError(ctypes.get_errno(),"sync_file_range")
                    for fd in descriptors:
                        t=time.monotonic_ns();os.fsync(fd);times.append((time.monotonic_ns()-t)/1e6)
                finally:
                    for fd in descriptors:os.close(fd)
            elapsed=(time.monotonic_ns()-started)/1e6
            for index in range(128):
                with open(f'{root}/{index}.r','rb') as f:
                    assert f.read()==b'x'*4096
            print(json.dumps(dict(sample=sample,batch=batch,writeback_hint=batch==16,files=128,total_ms=elapsed,sync_mean_ms=statistics.mean(times),sync_max_ms=max(times))),flush=True)
        finally:shutil.rmtree(root)
