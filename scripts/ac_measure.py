#!/usr/bin/env python3
"""AC fixed-rate rewrite measurement, 10Hz fresh stat RPC and bounded probes.
Run from the corresponding PRIVATE instrumented export on its assigned host.
The original and split sources use identical instrumentation and workload.
Raw files remain on host. Any payload/operation/cleanup error fails the trial.
"""
import argparse, hashlib, json, math, os, re, signal, statistics, sys, tempfile, threading, time, shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from k33_daemons import Cluster, Mount, mgmt
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',required=True,type=Path)
p.add_argument('--seconds',type=float,default=60)
p.add_argument('--address',default='127.0.0.1')
p.add_argument('--transport',choices=['tcp','rdma'],required=True)
p.add_argument('--rate',type=float,default=10,help='rewrites per writer per second')
p.add_argument('--writers',type=int,default=4)
p.add_argument('--variant',required=True)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
assert '/efs-task-ac' in str(ROOT) and a.seconds>=30 and a.rate>0
scratch=ROOT.parent/'tmp';scratch.mkdir(exist_ok=True)
work=Path(tempfile.mkdtemp(prefix='ac-',dir=scratch))
env=dict(os.environ,EFS_TRANSPORT=a.transport,EFS_RAFT_OBS='1')
env.pop('LD_PRELOAD',None)
c=Cluster(work,'cluster',[1,2,3],env,hosts={i:a.address for i in [1,2,3]})
m=None;errors=[];done=threading.Event();counts=[0]*a.writers;late=[0]*a.writers;lat=[]
bodies={};names=[];threads=[]
def cpu():
    result={}
    for i,proc in c.procs.items():
        fields=Path(f'/proc/{proc.pid}/stat').read_text().rsplit(')',1)[1].split()
        result[i]=(int(fields[11])+int(fields[12]))/os.sysconf('SC_CLK_TCK')
    return result

def writer(k):
    tick=start
    try:
        n=0
        while time.monotonic()<end and not done.is_set():
            name=names[k][n%len(names[k])]
            # Same fixed set, O_TRUNC + full replacement + fsync, no growth.
            with name.open('wb',buffering=0) as f:
                view=memoryview(bodies[name.name])
                while view:
                    nw=f.write(view);assert nw and nw>0;view=view[nw:]
                os.fsync(f.fileno())
            counts[k]+=1;n+=1;tick+=1/a.rate
            wait=tick-time.monotonic()
            if wait<0:late[k]+=1;tick=time.monotonic()
            else:done.wait(wait)
    except BaseException as e:errors.append(repr(e));done.set()

def statter():
    tick=start
    try:
        while time.monotonic()<end and not done.is_set():
            t=time.monotonic()
            # Attribute cache expiry is zero: each stat exercises client RPC.
            os.stat(names[0][0])
            lat.append((time.monotonic()-t)*1e6);tick+=.1
            done.wait(max(0,tick-time.monotonic()))
    except BaseException as e:errors.append(repr(e));done.set()

try:
    for i in [1,2,3]:c.start(i,join=1 if i>1 else None,quota='1G')
    c.wait_voters([1,2,3],lambda i,v:v=={0:(1,7),2:(1,7)})
    m=Mount(c,1,'mount')
    for k in range(a.writers):
        row=[]
        for n in range(32):
            name=f'ac-{k}-{n}';body=hashlib.shake_256(name.encode()).digest(65536)
            f=m.path/name;f.write_bytes(body);bodies[name]=body;row.append(f)
        names.append(row)
    for proc in c.procs.values():os.kill(proc.pid,signal.SIGUSR2)
    before=cpu();start=time.monotonic();end=start+a.seconds
    threads=[threading.Thread(target=writer,args=(k,)) for k in range(a.writers)]
    threads.append(threading.Thread(target=statter))
    for t in threads:t.start()
    for t in threads:t.join(timeout=a.seconds+30)
    assert all(not t.is_alive() for t in threads),'workload thread stuck'
    elapsed=time.monotonic()-start;after=cpu()
    for proc in c.procs.values():os.kill(proc.pid,signal.SIGUSR2)
    assert not errors,errors
    for row in names:
        for f in row:assert f.read_bytes()==bodies[f.name],f
    # Explicit management snapshot still supplies all five I/O classes + RDMA.
    replies={}
    for i in [1,2,3]:
        rc,text=mgmt(env,'io-stats',c.ep(i));assert rc==0,text
        replies[i]=text
        assert 'rdma' in text and 'disk_read' in text,text
    (a.out/'management.json').write_text(json.dumps(replies,indent=2))
    result=dict(variant=a.variant,transport=a.transport,seconds=elapsed,writes=counts,
                rate_late=late,mib_s=sum(counts)*65536/1048576/elapsed,
                stat_samples=len(lat),stat_p50_us=statistics.median(lat),
                stat_p99_us=sorted(lat)[math.ceil(.99*len(lat))-1],
                daemon_cpu_s={i:after[i]-before[i] for i in before},errors=errors)
finally:
    done.set()
    if m:m.close()
    c.stop_all()
    for t in threads:
        if t.is_alive():t.join(timeout=5)
# Probe output is emitted only at orderly process exit.
probe=[];obs=[]
for f in c.dir.glob('n*.log'):
    for line in f.read_text().splitlines():
        if 'ac-probe:' in line:probe.append(dict(node=f.stem,line=line))
        if 'raft-obs: wait_timeouts=' in line:obs.append(dict(node=f.stem,line=line))
result['probe']=probe;result['raft_obs']=obs;result['fixture']=str(work)
logs=a.out/'logs';logs.mkdir()
for f in c.dir.glob('*.log'):shutil.copy2(f,logs/f.name)
assert len(probe)>=3,'instrumentation missing'
assert all(n>=a.rate*a.seconds*.8 for n in counts),('offered rate not sustained',counts,late)
assert len(lat)>=a.seconds*8,('stat sampler insufficient',len(lat))
(a.out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
