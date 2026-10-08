"""Tiny payload overwrite/delete/recreate with cold identity verification."""
import concurrent.futures
import hashlib
import os
from pathlib import Path
import time

SIZES=(1,17,128,512,65536)

def body(worker,index,generation):
    size=SIZES[index%len(SIZES)]
    seed=hashlib.sha256(f'{worker}:{index}:{generation}'.encode()).digest()
    return (seed*((size+31)//32))[:size]

def exercise(mount):
    folder=Path(mount)/'tiny-churn';folder.mkdir()
    latency=[]
    def writer(worker):
        for index in range(25):
            path=folder/f'{worker}-{index}'
            for generation in range(3):
                started=time.monotonic_ns()
                flags=os.O_WRONLY|os.O_CREAT|(os.O_EXCL if generation!=1 else os.O_TRUNC)
                fd=os.open(path,flags,0o600)
                try:
                    data=body(worker,index,generation)
                    done=0
                    while done<len(data):
                        n=os.write(fd,data[done:]);assert n>0;done+=n
                    os.fsync(fd)
                finally:os.close(fd)
                latency.append((time.monotonic_ns()-started)/1000)
                assert path.read_bytes()==data,path
                if generation==1:path.unlink()
    started=time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(writer,range(4)))
    latency.sort()
    return {'files':100,'durable_generations':300,'sizes':SIZES,'wall_s':time.monotonic()-started,
            'write_fsync_close_p50_us':latency[len(latency)//2],
            'write_fsync_close_p99_us':latency[(len(latency)*99+99)//100-1]}

def verify_and_delete(mount):
    folder=Path(mount)/'tiny-churn'
    for worker in range(4):
        for index in range(25):
            path=folder/f'{worker}-{index}'
            assert path.read_bytes()==body(worker,index,2),path
            path.unlink()
    folder.rmdir()
