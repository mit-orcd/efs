#!/usr/bin/env python3
"""Real quota refusal followed by reclaim: same PUT must regain full protection.

Three private daemons, node 3 at 16 MiB; fill to its measured quota without a failed application
write. A new
2 MiB write waits on that quota, then four acknowledged files are unlinked.
Existing 45 s write bound; original PUT retry/deadline limits are unchanged.
Every retained acknowledged payload is checked before/after a cold restart.
Fixtures and logs are always kept; nothing formats an existing store.
"""
import json,os,re,subprocess,tempfile,threading,time
from pathlib import Path
from k33_daemons import Cluster,Mount,mgmt,fuse_ok

if not fuse_ok():
    print('PUT quota/reclaim: NOTRUN (FUSE unavailable)')
    raise SystemExit(0)
work=Path(tempfile.mkdtemp(prefix='efs-put-reclaim-',dir=os.environ.get('TMPDIR')))
env={k:v for k,v in os.environ.items() if not k.startswith('EFS_FAULT_')}
c=Cluster(work,'q',[1,2,3],env)
mounts=[];expected={};result={};writer=None

def payload(name,size):
    block=(name.encode()+b'\0')*128
    return (block*((size+len(block)-1)//len(block)))[:size]

def write(m,name,size):
    fd=os.open(m.path/name,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o644)
    try:
        data=payload(name,size);at=0
        while at<len(data):
            n=os.write(fd,data[at:]);assert n>0;at+=n
        os.fsync(fd)
    finally:os.close(fd)

def close(m):
    r=subprocess.run(['fusermount3','-u',str(m.path)],capture_output=True,timeout=30)
    assert r.returncode==0,r.stderr.decode(errors='replace')
    assert m.p.wait(timeout=30)==0 and not os.path.ismount(m.path)
    mounts.remove(m);m.log.close()

def stop():
    children=list(c.procs.values());c.stop_all()
    assert all(p.returncode==0 for p in children),'non-orderly stop'

def verify(m):
    for name,size in expected.items():assert (m.path/name).read_bytes()==payload(name,size),name

try:
    c.start(1);c.start(2,join=1);c.start(3,join=1,quota='16M')
    for i in (1,2,3):c.wait_log(i,r'raft-host: formed: 3 members')
    m=Mount(c,1,'writer');mounts.append(m)
    filled=[];quota=16<<20;charge=None
    def used_bytes():
        rc,text=mgmt(env,'gc-status',c.ep(3))
        match=re.search(r'\bdata_usage_bytes=(\d+)',text)
        assert rc==0 and match,text
        return int(match[1])
    used=used_bytes()
    for i in range(64):
        if charge is not None and quota-used<charge:break
        name=f'fill-{i}'
        write(m,name,1<<20)
        expected[name]=1<<20;filled.append(name)
        now=used_bytes();delta=now-used;assert delta>0
        charge=max(charge or 0,delta);used=now
    else:raise AssertionError('16 MiB member never filled')
    assert charge and 0<=quota-used<charge
    result.update(quota_bytes=quota,used_before_wait=used,file_charge_bytes=charge)
    assert len(filled)>=4
    result['acknowledged_fill_files']=len(filled)
    created=threading.Event();new_ino=[];errors=[]
    def waiting_write():
        try:
            fd=os.open(m.path/'after-reclaim',os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o644)
            try:
                new_ino.append(os.fstat(fd).st_ino);created.set()
                data=payload('after-reclaim',2<<20);at=0
                while at<len(data):
                    n=os.write(fd,data[at:]);assert n>0;at+=n
                os.fsync(fd)
            finally:os.close(fd)
        except BaseException as e:errors.append(repr(e));created.set()
    t=time.monotonic();writer=threading.Thread(target=waiting_write);writer.start()
    assert created.wait(5) and new_ino,errors
    marker=f'put quota backpressure ino={new_ino[0]} '
    end=time.monotonic()+5
    while marker not in m.logp.read_text(errors='replace'):
        assert writer.is_alive(),errors
        assert time.monotonic()<end,'same-ticket quota wait was not exercised'
        time.sleep(.01)
    result['real_quota_wait_exercised']=True
    for name in filled[:4]:(m.path/name).unlink();expected.pop(name)
    writer.join(45)
    assert not writer.is_alive() and not errors,errors
    result['write_s']=time.monotonic()-t;assert result['write_s']<=45
    expected['after-reclaim']=2<<20
    status_rc,status_text=mgmt(env,'status',c.ep(1))
    (work/'protection-before-cold.txt').write_text(status_text)
    assert status_rc==0 and 'Protection: full' in status_text,status_text
    verify(m);close(m);stop()
    for i in (1,2,3):c.start(i,join=None if i==1 else 1,quota='16M' if i==3 else '1G')
    c.wait_voters([1,2,3],lambda i,v:v=={0:(1,7),2:(1,7)},timeout=120)
    reader=Mount(c,2,'cold-reader');mounts.append(reader);verify(reader);close(reader);stop()
    result.update(verdict='PASS',exact_files=len(expected),method='measured quota -> actual PUT refusal/same-ticket wait -> unlink/reclaim -> full stripe, exact after whole-cluster restart')
finally:
    if writer is not None:writer.join(45)
    for m in list(mounts):m.close()
    c.stop_all()
    (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    (work/'expected.json').write_text(json.dumps(dict(acknowledged=expected,method='deterministic payload(name,size); setup failures are not acknowledged'),indent=2)+'\n')
    print('kept',work,flush=True)
print('PUT quota/reclaim: '+json.dumps(result),flush=True)
