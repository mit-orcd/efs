#!/usr/bin/env python3
"""W90 real-mount regression gates; nonzero exit for any required failure."""
import argparse,errno,json,os,time,traceback
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('mount');p.add_argument('peer');p.add_argument('--case',default='all');a=p.parse_args()
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
    d=root/'capacity';d.mkdir();(d/'capacity-file').write_bytes(b'x')
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
gate('snapshot',snapshot);gate('stats',stats);gate('invalid',invalid);gate('permissions',permissions);gate('fd_capacity',fd_capacity)
print(json.dumps(dict(root=str(root),results=results)),flush=True)
raise SystemExit(any(r['status']!='PASS' for r in results))
