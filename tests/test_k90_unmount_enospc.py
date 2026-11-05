#!/usr/bin/env python3
"""K90: an unmount after ENOSPC decides instead of waiting out the 60 s stop
bound. Three private daemons, node 3 at a small quota and filled through the
real refusal path (no hook). A 300000 B write is one W77 unit of three chunks;
its close answers ENOSPC with the 37856 B tail chunk still dirty.
- Mount A unmounts with node 3 still full: exit 1 within the bound below, the
  tail named by force-discard, no 60 s `UNMOUNT DATA LOSS`.
- Mount B holds the same refusal while node 3 restarts with room: exit 0, the
  tail lands, the unit publishes, the file reads back exact.
Skipped (NOTRUN) without FUSE.
"""
import errno,os,re,subprocess,tempfile,threading,time
from pathlib import Path
from k33_daemons import Cluster,Mount,fuse_ok,mgmt,payload

if not fuse_ok():
    print('K90 unmount after ENOSPC: NOTRUN (FUSE unavailable)')
    raise SystemExit(0)
QUOTA=4<<20
SIZE=300000
UNMOUNT_BOUND_S=30.0   # half the product stop bound; a held unit took all of it
work=Path(tempfile.mkdtemp(prefix='efs-k90-',dir=os.environ.get('TMPDIR')))
env={k:v for k,v in os.environ.items() if not k.startswith('EFS_FAULT_')}
env.update(EFS_PACK_MIN_AGE_MS='600000')  # the packer must not free room under the test
c=Cluster(work,'k90',[1,2,3],env)


def used():
    rc,text=mgmt(env,'gc-status',c.ep(3))
    m=re.search(r'\bdata_usage_bytes=(\d+)',text)
    assert rc==0 and m,text
    return int(m[1])


def refused_close(m,name,out):
    """One write request, no fsync: the refusal must reach close."""
    fd=os.open(m.path/name,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o644)
    out['ino']=os.fstat(fd).st_ino
    assert os.write(fd,payload(name,SIZE))==SIZE
    try:
        os.close(fd)
        out['errno']=0
    except OSError as e:
        out['errno']=e.errno


def unmount(m):
    t=time.monotonic()
    r=subprocess.run(['fusermount3','-u',str(m.path)],capture_output=True,timeout=30)
    assert r.returncode==0,r.stderr.decode(errors='replace')
    # A timeout here is the defect: the drain still waits for its bound.
    rc=m.p.wait(timeout=UNMOUNT_BOUND_S)
    dt=time.monotonic()-t
    assert not os.path.ismount(m.path)
    m.log.close()
    if m in c.mounts:
        c.mounts.remove(m)
    return rc,dt,m.logp.read_text(errors='replace')


try:
    c.start(1);c.start(2,join=1);c.start(3,join=1,quota=str(QUOTA>>20)+'M')
    for i in (1,2,3):c.wait_log(i,r'raft-host: formed: 3 members')
    a=Mount(c,1,'mnt-a');b=Mount(c,1,'mnt-b')
    # Fill: one more file of this size no longer fits on node 3.
    now=used();charge=0;fills=0
    while not charge or QUOTA-now>=charge:
        fd=os.open(a.path/f'fill-{fills}',os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o644)
        os.write(fd,payload(f'fill-{fills}',1<<20));os.fsync(fd);os.close(fd)
        fills+=1;after=used();charge=max(charge,after-now);now=after
        assert fills<16,'quota never filled'
    ra,rb={},{}
    ts=[threading.Thread(target=refused_close,args=(a,'held-a',ra)),
        threading.Thread(target=refused_close,args=(b,'held-b',rb))]
    t=time.monotonic()
    for x in ts:x.start()
    for x in ts:x.join()
    refusal_s=time.monotonic()-t
    assert ra['errno']==errno.ENOSPC and rb['errno']==errno.ENOSPC,(ra,rb)
    # Still full: the drain gets a fresh refusal and decides.
    rc,dt_full,log=unmount(a)
    assert rc==1,(rc,log[-3000:])
    assert 'UNMOUNT DATA LOSS' not in log,log[-3000:]
    assert 'efs-fuse: unmount: ENOSPC' in log,log[-3000:]
    assert re.search(rf'force-discard ino={ra["ino"]} ci=2 off=0 len={SIZE-(2<<17)} cause=unmount-enospc',log),log[-3000:]
    # Room again: the retained tail lands and the whole unit publishes.
    c.stop(3);c.start(3,quota='1G')
    c.wait_members([1,2,3],3)
    rc,dt_room,log=unmount(b)
    assert rc==0,(rc,log[-3000:])
    assert 'force-discard' not in log,log[-3000:]
    v=Mount(c,1,'mnt-v')
    assert (v.path/'held-b').read_bytes()==payload('held-b',SIZE)
    assert (v.path/'fill-0').read_bytes()==payload('fill-0',1<<20)
    # Atomic unit: the given-up file shows none of its chunks.
    assert (v.path/'held-a').stat().st_size==0
    assert unmount(v)[0]==0
    print(f'K90 unmount after ENOSPC: refusal at close {refusal_s:.1f} s, full member exit 1 in '
          f'{dt_full:.1f} s, room back exit 0 in {dt_room:.1f} s, exact PASS')
finally:
    c.stop_all()
