#!/usr/bin/env python3
"""Task FL (K106) probe: the three waits one halted member (SIGSTOP) causes.

Usage: halted_waits.py TREE a|b|r|c halt|nohalt [WINDOW_S]
(TREE built with `make all tests/faults/efsd`; TMPDIR holds the retained
fixture; on intel1 run through scripts/evidence/ff/port_range.py)

Three efsd (EFS_FAULTS build, tcp), one efs-fuse, then node 3 gets SIGSTOP.
The last line is `ROW {json}`.
  b  both groups led by node 1; 30 sequential 256 KiB file writes after the
     halt. Every PUT places a fragment on node 3, so each write pays the
     client's reconnect to it. ROW: seconds per write.
  r  as b, then SIGCONT after 6 writes and writes back to back for 8 s more.
     ROW: seconds from the SIGCONT to the last write that left a fragment for
     node 3 unacknowledged (the client log's `degraded acknowledgement` lines).
  a  both groups led by node 3 (the client has noted it as leader); right
     after the halt 3 mkdirs. The first is a request in flight to the member
     that stopped. ROW: seconds and errno per mkdir.
  c  needs a tree whose txn finisher is disabled (`h->fin_running = 0`), so
     only the GC thread's recovery resolves a stranded transaction: leaders on
     node 1, directory spread at 64, churn creates/unlinks (reap markers),
     halt, EFS_FAULT_DECIDE_LOST deposes the coordinator before its DECIDE.
     ROW: seconds from the fault firing to the first `txn-recover ... ABORT`
     line and the gc-pass reap_fence times."""
import json,os,re,signal,sys,tempfile,threading,time
from datetime import datetime,timezone
from pathlib import Path
root=Path(sys.argv[1]);mode=sys.argv[2];halt=sys.argv[3]=='halt'
window=float(sys.argv[4]) if len(sys.argv)>4 else 40
sys.path.insert(0,str(root/'tests'))
import k33_daemons as k
fx=Path(tempfile.mkdtemp(prefix=f'efs-fl-{mode}-',dir=os.environ.get('TMPDIR')));work=fx/'work';work.mkdir()
lost=work/'decide-lost'
env={x:v for x,v in os.environ.items() if not x.startswith('EFS_FAULT_')}
env.update({'EFS_TRANSPORT':'tcp'})
if mode=='c':env['EFS_FAULT_DIR_SPREAD_MIN']='64'
c=k.Cluster(work,'cluster',(1,2,3),env)
row=dict(mode=mode,halt=halt,fixture=str(fx))
def secs(f):
    t0=time.time()
    try:f();e=None
    except OSError as x:e=x.errno
    return round(time.time()-t0,2),e
try:
    for i in (1,2,3):c.start(i,join=None if i==1 else 1,extra_env={'EFS_FAULT_DECIDE_LOST':str(lost)} if mode=='c' else None)
    c.wait_voters((1,2,3),lambda i,v:v=={0:(1,7),2:(1,7)})
    k.place_leaders(c,3 if mode=='a' else 1,groups=(0,2),timeout=15)
    m=k.Mount(c,1,'mnt');d=m.path/'d';d.mkdir()
    if mode=='r':
        for i in range(8):(d/f'w{i}').write_bytes(os.urandom(256<<10))
        c.procs[3].send_signal(signal.SIGSTOP)
        row['write_s']=[secs(lambda i=i:(d/f'x{i:02d}').write_bytes(os.urandom(256<<10))) for i in range(6)]
        t_cont=time.time();c.procs[3].send_signal(signal.SIGCONT);row['t_cont']=t_cont
        n=0
        while time.time()<t_cont+8:
            (d/f'y{n:04d}').write_bytes(os.urandom(256<<10));n+=1
        row['writes_after_cont']=n
    elif mode=='b':
        for i in range(8):(d/f'w{i}').write_bytes(os.urandom(256<<10))
        if halt:c.procs[3].send_signal(signal.SIGSTOP)
        row['write_s']=[secs(lambda i=i:(d/f'x{i:02d}').write_bytes(os.urandom(256<<10))) for i in range(30)]
        row['sum_s']=round(sum(x[0] for x in row['write_s']),1);row['max_s']=max(x[0] for x in row['write_s'])
    elif mode=='a':
        for i in range(5):(d/f'p{i}').mkdir()
        if halt:c.procs[3].send_signal(signal.SIGSTOP)
        row['mkdir_s']=[secs(lambda i=i:(d/f'q{i}').mkdir()) for i in range(3)]
    else:
        for i in range(100):(d/f's{i:03d}').write_bytes(b'y')
        stop=threading.Event();ops=[]
        def churn():
            i=0
            while not stop.is_set():
                p=d/f'v{i:06d}'
                for kind,f in (('write',lambda:p.write_bytes(b'x')),('unlink',p.unlink)):
                    t0=time.time();e=None
                    try:f()
                    except OSError as x:e=x.errno
                    ops.append((kind,t0,time.time()-t0,e))
                i+=1
        th=threading.Thread(target=churn);th.start();time.sleep(3)
        if halt:c.procs[3].send_signal(signal.SIGSTOP)
        t_h=time.time();lost.write_text('')
        end=t_h+window
        while time.time()<end:
            t0=time.time();os.listdir(d);ops.append(('listing',t0,time.time()-t0,None))
        if halt:c.procs[3].send_signal(signal.SIGCONT)
        time.sleep(2);stop.set();th.join(60)
        row['max']={k2:round(max([o[2] for o in ops if o[0]==k2 and o[1]>=t_h-1] or [0]),2) for k2 in ('write','unlink','listing')}
        row['errors']=[(o[0],o[3]) for o in ops if o[3]]
finally:
    for p in c.procs.values():
        if p.poll() is None:p.send_signal(signal.SIGCONT)
    c.stop_all()
if mode=='r':
    lines=(c.dir/'mnt.log').read_text(errors='replace').splitlines()
    t=[datetime.fromisoformat(l[:23]+'+00:00').timestamp() for l in lines if 'degraded acknowledgement' in l and 'node 3' in l]
    row['last_degraded_after_cont_s']=round(max(t)-row['t_cont'],2) if t else None
    row['degraded_lines']=len(t)
if mode=='c':
    def ts(l):
        mt=re.match(r'(\d{4}-\d\d-\d\d[ T]\d\d:\d\d:\d\d(?:\.\d+)?)',l)
        return datetime.fromisoformat(mt.group(1)).replace(tzinfo=timezone.utc).timestamp() if mt else None
    lines=[l for i in (1,2,3) for l in (c.dir/f'n{i}.log').read_text(errors='replace').splitlines()]
    fire=[ts(l) for l in lines if 'decide-lost fault' in l and 'ready record' not in l]
    rec=sorted(ts(l) for l in lines if 'txn-recover' in l and 'ABORT' in l and ts(l))
    row['fire']=fire[0] if fire else None
    row['recover_after_fire_s']=round(rec[0]-fire[0],1) if fire and fire[0] and rec else None
    row['reap_fence_ms']=[int(x) for l in lines if 'gc-pass' in l for x in re.findall(r'reap_fence=(\d+)',l)]
    row['txn_recover_lines']=[l[:160] for l in lines if 'txn-recover' in l][:4]
print('ROW',json.dumps(row))
