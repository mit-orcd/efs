#!/usr/bin/env python3
"""Serialized same-binary 15-minute feature-off/on comparisons; preserve stores."""
import json,subprocess,time,os,sys,gzip,hashlib,re
from pathlib import Path
root=Path(__file__).resolve().parent
source=Path('/data1/efs/w97-flow-build')
trials=[('baseline',0,0),('noop',1,0),('both',1,1),('group',0,1)]
assert subprocess.check_output([source/'efsd','--version']).decode().startswith('efsd 0bd7b5be ')
(root/'versions.json').write_text(json.dumps(dict(version=subprocess.check_output([source/'efsd','--version']).decode(),
    sha256={name:hashlib.sha256((source/name).read_bytes()).hexdigest() for name in ('efsd','efs-fuse','efs-mgmt')}),indent=2))
for tag,noop,group in trials:
    flow=1
    os.environ.update(EFS_RAFT_BATCH_NOOP=str(noop),EFS_RAFT_GROUP_DRAIN=str(group))
    cell=root/tag;cell.mkdir();start=time.monotonic()
    print('START',tag,'noop',noop,'group',group,flush=True)
    subprocess.run(['bash',root/'restart.sh',str(flow),'1',cell],check=True)
    (cell/'deployed-version.txt').write_text(subprocess.check_output(['/data1/efs/src/efsd','--version']).decode())
    # Ensure recorded processes use this mode, not a retained pre-restart process.
    pids=subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)']).decode().split()
    assert len(pids)==4
    for pid in pids:
        env=dict(x.split('=',1) for x in Path(f'/proc/{pid}/environ').read_bytes().decode().split('\0') if '=' in x)
        assert env['EFS_RAFT_FLOW']==str(flow) and env['EFS_BATCH_OBS']=='1'
        assert env['EFS_RAFT_BATCH_NOOP']==str(noop) and env['EFS_RAFT_GROUP_DRAIN']==str(group)
    text=(root/'template-capture.py').read_text().replace('/data1/efs/w97-flow-smoke-20261009',str(cell))
    text=text.replace('tiny-w97-flow-smoke-20261009',f'tiny-w93-batch-hour-{tag}-20261009')
    text=text.replace('/data1/efs/w97-replication-loader.py',str(root/'loader-latency.py'))
    # Five-minute exploratory factor-isolation runs.
    # Observe leader term and committed/apply lag once per 100-second window.
    needle="        f.write(json.dumps(s)+'\\n');f.flush()"
    addition='''        if i%20==0:
            s['raft_status']={}
            for node in range(1,5):
                try:
                    r=subprocess.run(['/data1/efs/src/efs-mgmt','raft-status',f'127.0.0.1:{17431+node}'],capture_output=True,text=True,timeout=3)
                    s['raft_status'][node]=dict(returncode=r.returncode,stdout=r.stdout,stderr=r.stderr)
                except subprocess.TimeoutExpired:s['raft_status'][node]=dict(timeout=True)
'''
    assert needle in text
    text=text.replace(needle,addition+needle)
    (cell/'capture.py').write_text(text)
    env=dict(os.environ,W97_LATENCY_PATH=str(cell/'latency.json'))
    subprocess.run(['python3',cell/'capture.py'],env=env,check=True)
    text=(root/'template-analyze.py').read_text().replace("[('all',0,300),('early',0,100),('middle',100,200),('late',200,300)]", "[('all',0,300)]+[(f'w{i//100+1}',i,i+100) for i in range(0,300,100)]")
    text=text.replace('[(0,100),(100,200),(200,300)]','[(i,i+100) for i in range(0,300,100)]')
    (cell/'analyze.py').write_text(text)
    with (cell/'analysis.txt').open('w') as log:subprocess.run(['python3',cell/'analyze.py'],stdout=log,check=True)
    # Quantiles use bin upper bounds; final 100s bucket can include drain.
    raw=json.loads((cell/'latency.json').read_text());assert raw['workers'];merged={}
    for rows in raw['workers'].values():
        for win,row in rows.items():
            out=merged.setdefault(win,dict(n=0,sum_us=0,max_us=0,bins=[0]*25))
            out['n']+=row['n'];out['sum_us']+=row['sum_us'];out['max_us']=max(out['max_us'],row['max_us'])
            out['bins']=[a+b for a,b in zip(out['bins'],row['bins'])]
    for row in merged.values():
        for q in (50,95,99):
            target=(row['n']*q+99)//100;seen=0
            for i,n in enumerate(row['bins']):
                seen+=n
                if seen>=target:row[f'p{q}_upper_us']=0 if i==0 else (2**(i-1) if i<24 else None);break
    (cell/'latency-summary.json').write_text(json.dumps(merged,indent=2))
    cut={};observations=[]
    def statuses():
        rows=[]
        for node in range(1,5):
            raw=subprocess.check_output(['/data1/efs/src/efs-mgmt','raft-status',f'127.0.0.1:{17431+node}'],timeout=10).decode()
            for m in re.finditer(r'group (\d+) hosted=(\d+) role=(\w+) leader=(-?\d+) term=(\d+) commit=(\d+) applied=(\d+)',raw):
                rows.append(dict(node=node,group=int(m[1]),hosted=int(m[2]),role=m[3],leader=int(m[4]),term=int(m[5]),commit=int(m[6]),applied=int(m[7])))
        return rows
    first=statuses()
    for row in first:
        if row['hosted'] and row['role']=='leader':cut[row['group']]=row['commit']
    # CLI role names are lower-case; tolerate upper-case for older tools.
    if len(cut)!=2:
        cut={r['group']:r['commit'] for r in first if r['hosted'] and r['role'].lower()=='leader'}
    assert len(cut)==2,first
    caught=False
    for attempt in range(120):
        rows=statuses();observations.append(rows)
        caught=all(r['applied']>=cut[r['group']] and r['commit']>=cut[r['group']] for r in rows if r['hosted'])
        if caught:break
        time.sleep(.5)
    (cell/'catch-up.json').write_text(json.dumps(dict(cut=cut,caught_up=caught,observations=observations),indent=2))
    assert caught,'hosted followers did not apply the sampled leader cut after drain'
    for path in cell.glob('n*-window.log'):
        with gzip.open(str(path)+'.gz','wb') as out:out.write(path.read_bytes())
        path.unlink()
    (cell/'trial.json').write_text(json.dumps(dict(tag=tag,flow=flow,noop=noop,group=group,elapsed_including_restart=time.monotonic()-start),indent=2))
    print('DONE',tag,(cell/'rates.json').read_text().replace('\n',''),flush=True)
os.environ.update(EFS_RAFT_BATCH_NOOP='0',EFS_RAFT_GROUP_DRAIN='0')
end=root/'final';end.mkdir();subprocess.run(['bash',root/'restart.sh','1','0',end],check=True)
(root/'complete.json').write_text(json.dumps(dict(trials=trials,finished_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))))
print('four five-minute exploratory cases complete',flush=True)
