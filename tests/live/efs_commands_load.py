#!/usr/bin/env python3
"""Bounded private-fixture writer/publication gate; never use an existing tree."""
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import hashlib
import statistics
from query_rpc import query, request

mount=Path(sys.argv[1])
peer=Path(sys.argv[2])
fixture=os.environ.get('EFS_QUERY_TEST_FIXTURE','command-load')
assert fixture and '/' not in fixture and fixture not in ('.','..')
root=mount/fixture
root.mkdir()  # refuse reuse
root_ino=root.stat().st_ino
end=time.monotonic()+14
latencies=[[] for _ in range(8)]

def writer(worker):
    count=0
    folder=root/str(worker)
    folder.mkdir()
    while time.monotonic()<end and count<2000:
        # Actual small payload and close, rather than namespace-only RPCs.
        start=time.monotonic()
        (folder/str(count)).write_bytes(bytes([worker])*256)
        latencies[worker].append((time.monotonic()-start)*1000)
        count+=1
    return count

def reader():
    # Two independent RPC readers overlap each other and the refresh worker.
    # Small explicit buffers avoid exhausting the separate reply-memory cap.
    successes=0
    generations=set()
    sampled=[]
    busy=0
    while time.monotonic()<end:
        started=time.monotonic()
        reply,text=query(int(os.environ['EFS_QUERY_TEST_PORT']),request(
            version=2,operation=1,root=root_ino,uid=os.getuid(),gid=os.getgid(),match=3,term=b'',max_output=4<<20))
        if len(sampled)<4096:sampled.append((time.monotonic()-started)*1000)
        if reply.status:
            busy+=1
            assert reply.status==-13 and not text,('concurrent query failure',reply.status)
            continue  # bounded BUSY during mutations; never partial success
        result=json.loads(text)
        paths=[bytes.fromhex(entry['path_hex']) for entry in result['entries']]
        assert result['complete'] and len(paths)==len(set(paths)),result
        assert result['snapshot']['generation']==reply.generation
        generations.add(reply.generation)
        successes+=1
    ordered=sorted(sampled)
    def pct(p):return ordered[min(len(ordered)-1,int((len(ordered)-1)*p))] if ordered else 0
    return successes,sorted(generations),dict(sampled_requests=len(ordered),busy_requests=busy,
        query_p50_ms=pct(.5),query_p95_ms=pct(.95),query_p99_ms=pct(.99))

generations=[]
with concurrent.futures.ThreadPoolExecutor(max_workers=10) as pool:
    tasks=[pool.submit(writer,i) for i in range(8)]
    readers=[pool.submit(reader) for _ in range(2)]
    for _ in range(5):
        time.sleep(2)
        result=subprocess.run([sys.executable,str(peer/'.efs/stats'),'--json'],
                              check=True,stdout=subprocess.PIPE,timeout=12)
        snap=json.loads(result.stdout)['snapshot']
        generations.append(snap['generation'])
        print('loaded snapshot '+json.dumps(snap,sort_keys=True),flush=True)
    counts=[t.result(timeout=30) for t in tasks]
    reader_results=[t.result(timeout=30) for t in readers]
assert all(count>1 and len(gens)>1 for count,gens,_ in reader_results),reader_results
print('concurrent readers and publication PASS '+str(reader_results),flush=True)
assert sum(counts)>100,counts
assert len(set(generations))>1,('publication starved under ordinary writes',generations)
# After close, wait for the exact independent writer oracle.
expected=sum(counts)
converge_start=time.monotonic()
for _ in range(40):
    result=subprocess.run([sys.executable,str(peer/fixture/'.efs/stats'),'--json'],
                          check=True,stdout=subprocess.PIPE,timeout=12)
    parsed=json.loads(result.stdout)
    # A published cut can include the last CREATE before its size update.
    # Counts and logical bytes must converge together before the payload oracle.
    if (parsed['stats']['tree_files']==expected and
            parsed['stats']['tree_bytes']==expected*256):
        break
    time.sleep(.25)
else:
    raise AssertionError(('closed writer oracle did not publish',expected,parsed))
print(f'loaded publication PASS files={expected} logical_bytes={expected*256}',flush=True)

converge=time.monotonic()-converge_start
def verify_payload_folder(folder, worker, count):
    expected_hash=hashlib.sha256(bytes([worker])*256).digest()
    for index in range(count):
        payload=(folder/str(index)).read_bytes()
        assert len(payload)==256 and hashlib.sha256(payload).digest()==expected_hash,(worker,index)
    return count


def verify_payloads(peer, fixture, counts):
    # Every file remains in the oracle. Four readers bound descriptor and RPC
    # pressure while avoiding a serial round trip for each tiny file.
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        tasks=[pool.submit(verify_payload_folder,peer/fixture/str(worker),worker,count)
               for worker,count in enumerate(counts)]
        return sum(task.result() for task in tasks)


verified=verify_payloads(peer,fixture,counts)
assert verified==expected,(verified,expected)
values=sorted(value for worker in latencies for value in worker)
def percentile(p):return values[min(len(values)-1,int((len(values)-1)*p))]
print('writer latency and exact payload oracle '+json.dumps(dict(
    fixture=fixture,files=expected,create_p50_ms=statistics.median(values),
    create_p95_ms=percentile(.95),create_p99_ms=percentile(.99),
    converge_seconds=converge,payloads_verified=verified),sort_keys=True),flush=True)
