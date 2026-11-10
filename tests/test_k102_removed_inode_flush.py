#!/usr/bin/env python3
"""K102: write-back of a removed inode is retired, and a stop names what refuses it.

Three private daemons, node 3 at a small quota and filled through the real
refusal path (as tests/test_k90_unmount_enospc.py). A 300000 B write is one
W77 unit of three chunks; its close answers ENOSPC with the tail chunk still
dirty on mount A. Another mount then removes the file, so the server answers
NOT_FOUND for everything mount A still holds for it.
- Before the fix mount A retried that chunk on every pass of the stop drain
  (`dcache-flush-fail ino=N stage=put rc=-3`, about 100 a second), the stop
  was refused after 60 s and the refusal named nothing (both counts 0).
- Now each chunk is retired once (`dcache-inode-gone`), the stop succeeds and
  mount A unmounts clean (K90 keeps QUOTA's meaning for files that exist).
Skipped (NOTRUN) without FUSE.
"""
import errno,os,re,subprocess,tempfile,time
from pathlib import Path
from k33_daemons import Cluster,Mount,FUSE,fuse_ok,mgmt,payload

if not fuse_ok():
    print('K102 removed inode flush: NOTRUN (FUSE unavailable)')
    raise SystemExit(0)
QUOTA=4<<20
SIZE=300000
STOP_BOUND_S=20.0      # the product stop bound is 60 s; a retired chunk needs a pass or two
work=Path(tempfile.mkdtemp(prefix='efs-k102-',dir=os.environ.get('TMPDIR')))
env={k:v for k,v in os.environ.items() if not k.startswith('EFS_FAULT_')}
env.update(EFS_PACK_MIN_AGE_MS='600000')  # the packer must not free room under the test
c=Cluster(work,'k102',[1,2,3],env)


def used():
    rc,text=mgmt(env,'gc-status',c.ep(3))
    m=re.search(r'\bdata_usage_bytes=(\d+)',text)
    assert rc==0 and m,text
    return int(m[1])


def refused_close(m,name):
    """One write request, no fsync: the refusal reaches close, the tail stays dirty."""
    fd=os.open(m.path/name,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o644)
    ino=os.fstat(fd).st_ino
    assert os.write(fd,payload(name,SIZE))==SIZE
    try:
        os.close(fd)
        raise AssertionError('close was not refused')
    except OSError as e:
        assert e.errno==errno.ENOSPC,e
    return ino


try:
    c.start(1);c.start(2,join=1);c.start(3,join=1,quota=str(QUOTA>>20)+'M')
    for i in (1,2,3):c.wait_log(i,r'raft-host: formed: 3 members')
    a=Mount(c,1,'mnt-a');b=Mount(c,1,'mnt-b')
    now=used();charge=0;fills=0
    while not charge or QUOTA-now>=charge:
        fd=os.open(a.path/f'fill-{fills}',os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o644)
        os.write(fd,payload(f'fill-{fills}',1<<20));os.fsync(fd);os.close(fd)
        fills+=1;after=used();charge=max(charge,after-now);now=after
        assert fills<16,'quota never filled'
    gone=refused_close(a,'gone')
    # Mount B removes the file: mount A is not told and keeps the dirty tail.
    (b.path/'gone').unlink()
    assert not (b.path/'gone').exists()
    t=time.monotonic()
    stop=subprocess.Popen([str(FUSE),'--stop',str(a.path)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    try:
        out,_=stop.communicate(timeout=STOP_BOUND_S)
    except subprocess.TimeoutExpired:
        # The defect: show how it ends (refused after the 60 s bound) and fail.
        out,_=stop.communicate(timeout=90)
        time.sleep(2)   # the refusal line follows the control reply
        log=a.text()
        n=len(re.findall(rf'dcache-flush-fail ino={gone} stage=put rc=-(?:3|14)\b',log))
        refused=re.findall(r'stop-refused[^\n]*',log)
        raise AssertionError(f'stop of mount A refused (rc {stop.returncode}) after {time.monotonic()-t:.0f} s: '
                             f'{n} retries of removed inode {gone}; {refused}; {out.strip()}')
    took=time.monotonic()-t
    log=a.text()
    assert stop.returncode==0,(stop.returncode,out,log[-3000:])
    assert 'stop-refused' not in log,log[-3000:]
    # Each chunk of the removed inode is retired once: the tail by NOT_FOUND
    # (merge-base fetch), the full chunks by STALE (ticket of a removed FileID).
    for ci in (0,1,2):
        assert len(re.findall(rf'dcache-inode-gone ino={gone} ci={ci}:',log))==1,(ci,log[-3000:])
    # The refused close logged rc=-11 before the removal. At most the pass that
    # found the inode gone may log NOT_FOUND/STALE; never a retry loop.
    retries=len(re.findall(rf'dcache-flush-fail ino={gone} stage=put rc=-(?:3|14)\b',log))
    assert retries<=1,(retries,log[-3000:])
    r=subprocess.run(['fusermount3','-u',str(a.path)],capture_output=True,timeout=30)
    assert r.returncode==0,r.stderr.decode(errors='replace')
    assert a.p.wait(timeout=30)==0,a.text()[-3000:]
    log=a.text()
    assert 'UNMOUNT DATA LOSS' not in log and 'unmount did not complete cleanly' not in log,log[-3000:]
    a.log.close();c.mounts.remove(a)
    print(f'K102 removed inode flush: each chunk retired once, stop ok in {took:.1f} s, mount A exit 0, '
          f'{retries} retry lines PASS')
finally:
    c.stop_all()
