#!/usr/bin/env python3
"""Task FF (K104) probe: strand a metadata transaction with one member halted.

Usage: halted_strand.py TREE halt|nohalt [WINDOW_S]   (TREE built with
`make all tests/faults/efsd`; TMPDIR holds the retained fixture)

Three efsd (EFS_FAULTS build) and one efs-fuse; both Raft groups led by node 1;
a 100-entry directory spread at 64 entries; a thread creates and unlinks one
file at a time. Then node 3 gets SIGSTOP ("halt") and EFS_FAULT_DECIDE_LOST
deposes the coordinator of the next multi-shard transaction before its DECIDE
(the leader-change race of tests/test_ao_readdir_live.py, made certain).
Listings run for WINDOW_S (default 40) seconds, then SIGCONT. The last line
is `ROW {json}`: errors, longest unlink/write/listing, fault time, fixture.
Before the Task FF fix: an unlink returns EBUSY after 30.0 s and the listing
waits 30 s behind it; after it: about 1 s. "nohalt" is the control."""
import json,os,signal,sys,tempfile,threading,time
from pathlib import Path
root=Path(sys.argv[1]);halt=sys.argv[2]=='halt';window=float(sys.argv[3]) if len(sys.argv)>3 else 40
sys.path.insert(0,str(root/'tests'))
import k33_daemons as k
fx=Path(tempfile.mkdtemp(prefix='efs-ff-strand-',dir=os.environ.get('TMPDIR')));work=fx/'work';work.mkdir()
lost=work/'decide-lost'
env={x:v for x,v in os.environ.items() if not x.startswith('EFS_FAULT_')}
spread='EFS_FAULT_DIR_SPREAD_MIN' if 'EFS_FAULT_DIR_SPREAD_MIN' in (root/'src/meta/dir_layout.c').read_text() else 'EFS_DIR_SPREAD_MIN'
env.update({'EFS_TRANSPORT':'tcp',spread:'64'})
c=k.Cluster(work,'cluster',(1,2,3),env)
ops=[];errors=[];stop=threading.Event();res=dict(halt=halt)
try:
    for i in (1,2,3):c.start(i,join=None if i==1 else 1,extra_env={'EFS_FAULT_DECIDE_LOST':str(lost)})
    c.wait_voters((1,2,3),lambda i,v:v=={0:(1,7),2:(1,7)})
    k.place_leaders(c,1,groups=(0,2),timeout=15)
    m=k.Mount(c,1,'mnt');d=m.path/'d';d.mkdir()
    for i in range(100):(d/f's{i:03d}').write_bytes(b'y')
    def churn():
        i=0
        while not stop.is_set():
            p=d/f'v{i:06d}'
            for kind,f in (('write',lambda:p.write_bytes(b'x')),('unlink',p.unlink)):
                t0=time.time()
                try:f()
                except OSError as e:errors.append((kind,round(t0,3),repr(e)))
                ops.append((kind,t0,time.time()-t0))
            i+=1
    th=threading.Thread(target=churn);th.start();time.sleep(3)
    if halt:c.procs[3].send_signal(signal.SIGSTOP)
    t_h=time.time();lost.write_text('');res['t_halt']=t_h
    end=t_h+window
    while time.time()<end:
        t0=time.time();os.listdir(d);ops.append(('listing',t0,time.time()-t0))
    if halt:c.procs[3].send_signal(signal.SIGCONT)
    res['t_cont']=time.time();time.sleep(2);stop.set();th.join(60)
    res['fired']=Path(str(lost)+'.ready').read_text() if Path(str(lost)+'.ready').exists() else None
finally:
    stop.set()
    for p in c.procs.values():
        if p.poll() is None:p.send_signal(signal.SIGCONT)
    c.stop_all()
w=[o for o in ops if o[1]>=res['t_halt']-1]
res['errors']=errors;res['max']={k2:max([o[2] for o in w if o[0]==k2] or [0]) for k2 in ('write','unlink','listing')}
res['slow']=[(o[0],round(o[1]-res['t_halt'],2),round(o[2],2)) for o in w if o[2]>1]
res['n_ops']=len(ops)
res['gc']=[l for i in (1,2,3) for l in (c.dir/f'n{i}.log').read_text(errors='replace').splitlines() if 'gc-pass' in l or 'decide-lost' in l or 'unresponsive' in l]
fire=None
for l in res['gc']:
    if 'decide-lost fault' in l:fire=l[:24]
row=dict(halt=halt,fired=bool(res['fired']),errors=len(errors),err=errors[:2],max_unlink=round(res['max']['unlink'],2),max_write=round(res['max']['write'],2),max_listing=round(res['max']['listing'],2),fire=fire,fixture=str(fx))
print('ROW',json.dumps(row))
