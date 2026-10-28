#!/usr/bin/env python3
"""AF measurement, not full D4 qualification: fixed-rate D4 working-set rewrites.

Fresh three-member TCP/O_DIRECT fixture; packing/index/KV maintenance active.
One mount, four writers, 32 files each, D4 overwrite/truncate/in-place cycle,
exact versioned payloads and fsync. Poll every member at the requested rate.
All errors fail; fixtures and raw outputs stay on the originating host.
"""
import argparse,json,math,os,re,shutil,signal,statistics,sys,tempfile,threading,time,traceback
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'tests'),str(ROOT/'scripts')]
from k33_daemons import Cluster,Mount,mgmt
from d4_progress import payload,parse_pack
from leader_placement import parse_raft_status
from qualify import parse_raft
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True);p.add_argument('--hz',type=int,choices=[1,10,50],required=True)
p.add_argument('--seconds',type=float,default=60);p.add_argument('--rate',type=float,default=5)
a=p.parse_args();assert '/efs-task-af/' in str(ROOT) and a.seconds>=60 and a.rate>0
a.out.mkdir(parents=True,exist_ok=False)
scratch=ROOT.parent/'tmp';scratch.mkdir(exist_ok=True)
work=Path(tempfile.mkdtemp(prefix='af-',dir=scratch))
env={k:v for k,v in os.environ.items() if not k.startswith(('EFS_FAULT_','EFS_TEST_'))}
env.update(EFS_TRANSPORT='tcp',EFS_RAFT_OBS='1',EFS_PACK_MIN_AGE_MS='1000',
           EFS_PACK_INTERVAL_MS='200',EFS_QUERY_REFRESH_MS='2000',EFS_FAULT_MEMTABLE_BYTES='262144')
env.pop('LD_PRELOAD',None);env.pop('EFS_GC_DISABLE',None)
c=Cluster(work,'cluster',[1,2,3],env);mount=None;threads=[];stop=threading.Event();errors=[]
counts=[0]*4;late=[0]*4;versions={};names=[];stat_us=[];status_us={i:[] for i in [1,2,3]}
poll_late={i:0 for i in [1,2,3]};last_status={};doc=dict(hz=a.hz,fixture=str(work),errors=errors)
def quantiles(values):
    assert values
    return dict(count=len(values),p50_us=statistics.median(values),p99_us=sorted(values)[math.ceil(.99*len(values))-1],max_us=max(values))
def cpu():
    out={}
    for i,pr in c.procs.items():
        f=Path(f'/proc/{pr.pid}/stat').read_text().rsplit(')',1)[1].split()
        out[i]=(int(f[11])+int(f[12]))/os.sysconf('SC_CLK_TCK')
    return out
def pace(tick,period):
    tick+=period;remaining=tick-time.monotonic()
    if remaining<0:return time.monotonic(),True
    stop.wait(remaining);return tick,False
def writer(k):
    tick=start;n=0
    try:
        while time.monotonic()<end and not stop.is_set():
            f=names[k][n%32];v=versions[f]+1;data=payload(f'{f.name}#{v}',65536);op=n%3
            with f.open('wb' if op==0 else 'r+b',buffering=0) as stream:
                if op==1:os.ftruncate(stream.fileno(),32768)
                view=memoryview(data)
                while view:
                    nw=stream.write(view);assert nw and nw>0;view=view[nw:]
                if op==1:stream.truncate()
                os.fsync(stream.fileno())
            versions[f]=v;counts[k]+=1;n+=1;tick,waslate=pace(tick,1/a.rate);late[k]+=waslate
    except BaseException as e:errors.append(f'writer{k}: {e!r}');stop.set()
def poller(i):
    tick=start
    try:
        while time.monotonic()<end and not stop.is_set():
            t=time.monotonic();rc,text=mgmt(env,'raft-status',c.ep(i));elapsed=(time.monotonic()-t)*1e6
            assert rc==0,text
            lp=parse_raft_status(text);node,groups=parse_raft(text)
            assert lp['node']==node==i and set(groups)=={0,2} and set(lp['groups'])=={0,2},text
            status_us[i].append(elapsed);last_status[i]=text
            tick,waslate=pace(tick,1/a.hz);poll_late[i]+=waslate
    except BaseException as e:errors.append(f'poller{i}: {e!r}');stop.set()
def statter():
    tick=start
    try:
        while time.monotonic()<end and not stop.is_set():
            t=time.monotonic();os.stat(names[0][0]);stat_us.append((time.monotonic()-t)*1e6)
            tick,_=pace(tick,.1)
    except BaseException as e:errors.append(f'stat: {e!r}');stop.set()
ok=False
try:
    for i in [1,2,3]:c.start(i,join=1 if i>1 else None,quota='1G')
    c.wait_voters([1,2,3],lambda i,v:v=={0:(1,7),2:(1,7)})
    mount=Mount(c,1,'mount')
    for k in range(4):
        row=[]
        for n in range(32):
            f=mount.path/f'af-{k}-{n}';f.write_bytes(payload(f'{f.name}#0',65536));versions[f]=0;row.append(f)
        names.append(row)
    for proc in c.procs.values():os.kill(proc.pid,signal.SIGUSR2)
    before=cpu();start=time.monotonic();end=start+a.seconds
    threads=[threading.Thread(target=writer,args=(k,)) for k in range(4)]
    threads += [threading.Thread(target=poller,args=(i,)) for i in [1,2,3]]+[threading.Thread(target=statter)]
    for t in threads:t.start()
    for t in threads:t.join(timeout=a.seconds+60)
    assert all(not t.is_alive() for t in threads),'stuck workload'
    elapsed=time.monotonic()-start;after=cpu()
    for proc in c.procs.values():os.kill(proc.pid,signal.SIGUSR2)
    assert not errors,errors
    for f,v in versions.items():assert f.read_bytes()==payload(f'{f.name}#{v}',65536),f
    packing={}
    for i in [1]:
        rc,text=mgmt(env,'pack-compact',c.ep(i),'0');assert rc==0,text
        packing[i]=parse_pack(text)
        assert packing[i]['put_errors']==packing[i]['read_errors']==0,text
        packing[i]['led']=sorted(packing[i]['led'])
    doc.update(seconds=elapsed,writes=counts,write_late=late,mib_s=sum(counts)/16/elapsed,
               daemon_cpu_s={i:after[i]-before[i] for i in before},stat=quantiles(stat_us),
               status={i:quantiles(v) for i,v in status_us.items()},poll_late=poll_late,verified_files=len(versions),packing=packing)
    assert all(n>=a.seconds*a.rate*.8 for n in counts),(counts,late)
    assert len(stat_us)>=a.seconds*8,len(stat_us)
    assert all(len(v)>=a.seconds*a.hz*.8 for v in status_us.values()),{i:len(v) for i,v in status_us.items()}
    (a.out/'last-status.json').write_text(json.dumps(last_status,indent=2))
    ok=True
except BaseException:
    doc['failure']=traceback.format_exc();raise
finally:
    stop.set()
    if mount:mount.close()
    c.stop_all()
    logs=a.out/'logs';logs.mkdir()
    for f in c.dir.glob('*.log'):shutil.copy2(f,logs/f.name)
    doc['probe']=[];doc['raft_obs']=[]
    for f in logs.glob('n*.log'):
        for line in f.read_text().splitlines():
            if 'af-probe' in line:doc['probe'].append(dict(node=f.stem,line=line))
            if 'raft-obs: wait_timeouts=' in line:doc['raft_obs'].append(dict(node=f.stem,line=line))
    doc['verdict']='PASS' if ok else 'FAIL';(a.out/'result.json').write_text(json.dumps(doc,indent=2)+'\n')
print(json.dumps({k:v for k,v in doc.items() if k not in ['probe','raft_obs']},indent=2))
