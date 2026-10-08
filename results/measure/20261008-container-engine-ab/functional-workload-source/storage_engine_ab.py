#!/usr/bin/env python3
"""Fixed-client durable file workload for isolated files/containers server A/B."""
import argparse
import concurrent.futures
import hashlib
import json
import math
import os
from pathlib import Path
import random
import time

SIZE=512*1024
BLOCK=128*1024
COUNT=256
WORKERS=4
STEPS=64

def payload(seed,size):return random.Random(seed).randbytes(size)
def digest(data):return hashlib.sha256(data).hexdigest()
def write_all(fd,data,offset=0):
    done=0
    while done<len(data):
        n=os.pwrite(fd,memoryview(data)[done:],offset+done)
        if n<=0:raise RuntimeError('short write')
        done+=n

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('phase',choices=['create','read','churn','verify','delete'])
    p.add_argument('--mount',type=Path,required=True)
    p.add_argument('--state',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();directory=a.mount/'engine-ab'
    if a.phase=='create':
        directory.mkdir();state={'sizes':SIZE,'count':COUNT,'seed':42,'overwrites':[]}
    else:state=json.loads(a.state.read_text())
    expected=[bytearray(payload(42+i,SIZE)) for i in range(COUNT)] if a.phase in ('create','churn') else None
    if expected is not None:
        for i,offset,seed in state['overwrites']:expected[i][offset:offset+BLOCK]=payload(seed,BLOCK)
    replacements=[[payload(100000+w*STEPS+i,BLOCK) for i in range(STEPS)] for w in range(WORKERS)] if a.phase=='churn' else None
    def worker(w):
        lat=[];count=0;total=0;changes=[]
        for j in range(STEPS if a.phase=='churn' else COUNT//WORKERS):
            i=w+WORKERS*(j%8 if a.phase=='churn' else j)
            file=directory/f'{i:04}.dat'
            started=time.monotonic()
            if a.phase=='create':
                fd=os.open(file,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
                try:write_all(fd,expected[i]);os.fsync(fd)
                finally:os.close(fd)
                total+=SIZE
            elif a.phase=='churn':
                offset=(j//8)%4*BLOCK;data=replacements[w][j]
                fd=os.open(file,os.O_RDWR)
                try:write_all(fd,data,offset);os.fsync(fd)
                finally:os.close(fd)
                expected[i][offset:offset+BLOCK]=data;changes.append([i,offset,100000+w*STEPS+j]);total+=BLOCK
            elif a.phase=='delete':file.unlink()
            else:
                data=file.read_bytes()
                if len(data)!=SIZE or digest(data)!=state['hashes'][i]:raise RuntimeError(f'integrity mismatch {i}')
                total+=len(data)
            lat.append((time.monotonic()-started)*1000);count+=1
        return lat,count,total,changes
    begin=time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=WORKERS) as pool:results=list(pool.map(worker,range(WORKERS)))
    wall=time.monotonic()-begin
    lat=sorted(v for r in results for v in r[0]);count=sum(r[1] for r in results);total=sum(r[2] for r in results)
    if expected is not None:
        state['hashes']=[digest(v) for v in expected]
        state['overwrites'] += [c for r in results for c in r[3]]
        a.state.write_text(json.dumps(state)+'\n')
    if a.phase=='delete':directory.rmdir()
    row={'phase':a.phase,'workers':WORKERS,'operations':count,'bytes':total,'wall_s':wall,
         'MiB_s':total/1024**2/wall,'ops_s':count/wall,'p99_ms':lat[math.ceil(len(lat)*.99)-1],
         'max_ms':max(lat),'errors':0,'integrity':a.phase in ('read','verify')}
    a.output.write_text(json.dumps(row,indent=2)+'\n');print(json.dumps(row),flush=True)
if __name__=='__main__':main()
