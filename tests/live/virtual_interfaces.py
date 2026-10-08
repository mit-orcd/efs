#!/usr/bin/env python3
"""W90 real-mount regression gates; nonzero exit for any required failure."""
import argparse,errno,json,os,time,traceback
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('mount');p.add_argument('peer');p.add_argument('--case',default='all');p.add_argument('--freshness-s',type=float,default=10);a=p.parse_args()
name='w90-gates-'+str(os.getpid());root=Path(a.mount)/name;peer=Path(a.peer)/name;root.mkdir();results=[]
def gate(name,fn):
    if a.case not in ('all',name):return
    t=time.monotonic()
    try:fn();results.append(dict(case=name,status='PASS',seconds=round(time.monotonic()-t,3)))
    except Exception as e:results.append(dict(case=name,status='FAIL',error=repr(e),traceback=traceback.format_exc()))
    print(json.dumps(results[-1]),flush=True)
def contents(path):
    with open(path,'rb',buffering=0) as f:return f.read()
def snapshot():
    d=root/'snapshot';d.mkdir()
    for i in range(100): (d/f'match-{i:04}').write_bytes(b'x')
    query=d/'.find'/'match-*';expected=contents(query)
    fd=os.open(query,os.O_RDONLY)
    try:
        prefix=os.read(fd,17)
        (peer/'snapshot'/'match-new').write_bytes(b'y');time.sleep(5.2)
        # Evict all legacy result-cache slots while this fd remains open.
        for i in range(20):contents(d/'.find'/f'absent-{i:03}*')
        rest=b''
        while b:=os.read(fd,31):rest+=b
        assert prefix+rest==expected,(len(prefix+rest),len(expected))
        assert os.fstat(fd).st_size==len(expected)
        os.lseek(fd,0,0);assert os.read(fd,len(expected)+100)==expected
    finally:os.close(fd)
    assert b'match-new' in contents(query)
def stats():
    d=root/'stats';d.mkdir();(peer/'stats'/'payload').write_bytes(b'x'*4096);time.sleep(1.2)
    values=dict(line.split('=',1) for line in contents(d/'.stats').decode().splitlines())
    assert int(values['imm_files'])==1,values
    assert int(values['imm_bytes'])==4096,values
def stats_values(d):
    return dict(line.split('=',1) for line in contents(d/'.stats').decode().splitlines())
def expect_stats(d,**want):
    deadline=time.monotonic()+a.freshness_s
    while True:
        got=stats_values(d)
        if all(int(got[k])==v for k,v in want.items()):return got
        if time.monotonic()>=deadline:raise AssertionError(dict(expected=want,actual=got))
        time.sleep(.2)
def stats_mutations():
    d=root/'stats-mutations';d.mkdir();pd=peer/'stats-mutations'
    sub=pd/'sub';sub.mkdir();payload=sub/'payload'
    with open(payload,'wb') as f:
        f.write(b'x'*4096);f.flush();os.fsync(f.fileno())
    expect_stats(d,imm_files=0,imm_dirs=1,imm_bytes=0,tree_files=1,tree_dirs=1,tree_bytes=4096)
    os.link(payload,pd/'alias')
    expect_stats(d,imm_files=1,tree_files=2,tree_bytes=8192)
    with open(payload,'r+b') as f:
        f.truncate(1<<20);f.flush();os.fsync(f.fileno())
    expect_stats(d,imm_bytes=1<<20,tree_bytes=2<<20)
    # Pure directory rename must converge even without new payload writes.
    os.rename(sub,pd/'moved')
    expect_stats(d,imm_dirs=1,tree_files=2,tree_bytes=2<<20)
    assert b'moved/payload' in contents(d/'.find'/'payload')
    os.unlink(pd/'alias');os.unlink(pd/'moved'/'payload');os.rmdir(pd/'moved')
    expect_stats(d,imm_files=0,imm_dirs=0,imm_bytes=0,tree_files=0,tree_dirs=0,tree_bytes=0)
def invalid():
    try:contents(root/'.find'/'a*b*c')
    except OSError as e:assert e.errno==errno.EINVAL,e;return
    raise AssertionError('invalid query accepted')
def permissions():
    d=root/'protected';d.mkdir();(d/'secret-file').write_bytes(b'x');os.chmod(d,0o100)
    try:
        for q in [d/'.stats',d/'.find'/'secret-*']:
            try:contents(q)
            except OSError as e:assert e.errno==errno.EACCES,e
            else:raise AssertionError('protected query succeeded: '+str(q))
    finally:os.chmod(d,0o700)
def fd_capacity():
    d=root/'capacity';d.mkdir(exist_ok=True);(d/'capacity-file').write_bytes(b'x')
    q=d/'.find'/'capacity-*';expected=contents(q);fds=[];bounded=False
    try:
        for _ in range(80):
            try:fds.append(os.open(d/'.find'/f'absent-capacity-{_:03}*',os.O_RDONLY))
            except OSError as e:
                assert e.errno==errno.ENOMEM,(len(fds),e)
                bounded=True;break
        assert bounded and len(fds)>=20,len(fds)
        for fd in fds:assert os.read(fd,4096)==b''
    finally:
        for fd in fds:os.close(fd)
    assert contents(q)==expected
def recovery():
    d=root/'recovery';d.mkdir()
    for i in range(256):
        q=d/'.find'/f'absent-stat-{i:03}*'
        assert os.stat(q).st_size==0
    for i in range(128):
        assert contents(d/'.find'/f'absent-open-{i:03}*')==b''
    # Exhaust and drain twice, then use a different directory/query version.
    fd_capacity();fd_capacity();fd_capacity_repeat=d/'repeat';fd_capacity_repeat.mkdir()
    assert contents(fd_capacity_repeat/'.find'/'fresh-*')==b''
gate('recovery',recovery);gate('snapshot',snapshot);gate('stats',stats);gate('stats_mutations',stats_mutations);gate('invalid',invalid);gate('permissions',permissions);gate('fd_capacity',fd_capacity)
print(json.dumps(dict(root=str(root),results=results)),flush=True)
raise SystemExit(any(r['status']!='PASS' for r in results))
