#!/usr/bin/env python3
"""Exact single-description APPEND gate: each 4 KiB record has unique bytes."""
import hashlib,json,os,struct,time
from pathlib import Path

def body(index):
    return struct.pack('>Q',index)+hashlib.shake_256(b'W52-append-burst-v1'+struct.pack('>Q',index)).digest(4088)

def verify(path,count):
    assert path.stat().st_size==count*4096,(path,path.stat().st_size,count*4096)
    digest=hashlib.sha256()
    with path.open('rb',buffering=0) as source:
        for index in range(count):
            got=source.read(4096)
            assert got==body(index),('append block mismatch',str(path),index)
            digest.update(got)
        assert not source.read(1),'extra append bytes'
    return digest.hexdigest()

def run(mount,peer,count,work):
    path=mount/'append-burst';latency=[];windows=[];started=time.monotonic();checkpoint=started;previous=0
    fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_WRONLY|os.O_APPEND,0o600)
    try:
        for index in range(count):
            data=body(index);before=time.monotonic()
            assert os.write(fd,data)==4096,('short append',index)
            latency.append(time.monotonic()-before)
            now=time.monotonic()
            if now-checkpoint>=30:
                windows.append(dict(elapsed_s=now-started,records=index+1,records_per_s=(index+1-previous)/(now-checkpoint)))
                print('append burst',index+1,'/',count,'elapsed',round(now-started,3),flush=True)
                previous=index+1;checkpoint=now
        writes=time.monotonic()-started;before=time.monotonic();os.fsync(fd);sync=time.monotonic()-before
    finally:os.close(fd)
    expected=verify(path,count);assert verify(peer/path.name,count)==expected
    ordered=sorted(latency)
    report=dict(records=count,record_bytes=4096,exact_bytes=count*4096,write_seconds=writes,final_fsync_seconds=sync,
        final_fsync_returned_zero=True,write_latency_s={str(p):ordered[min(len(ordered)-1,int(p/100*len(ordered)))] for p in (50,95,99,100)},windows=windows,
        warm_primary_sha256=expected,warm_peer_sha256=expected)
    (work/'append-burst.json').write_text(json.dumps(report,indent=2))
    return path,report
