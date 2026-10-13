#!/usr/bin/env python3
"""Current .efs query gates; retired .find/.stats remain ordinary user names."""
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
def query_path(directory, pattern):
    raw = os.fsencode(pattern)
    mode = 0
    if raw.startswith(b'*') and raw.endswith(b'*'):
        mode, raw = 3, raw[1:-1]
    elif raw.endswith(b'*'):
        mode, raw = 1, raw[:-1]
    elif raw.startswith(b'*'):
        mode, raw = 2, raw[1:]
    return directory / '.efs/.query' / ('v1.find.%d.%s' % (mode, raw.hex()))
def find_bytes(path):
    value = json.loads(contents(path))
    assert value['complete'], value
    directory = path.parent.parent.parent
    return b''.join(os.fsencode(directory) + b'/' + bytes.fromhex(e['path_hex']) + b'\n' for e in value['entries'])
def contents(path):
    for attempt in range(8):
        try:
            with open(path,'rb',buffering=0) as f:return f.read()
        except OSError as error:
            if error.errno not in (errno.ESTALE,errno.EAGAIN,errno.EBUSY) or attempt==7:raise
            time.sleep(.025*(attempt+1))
def snapshot():
    d=root/'snapshot';d.mkdir()
    for i in range(100): (d/f'match-{i:04}').write_bytes(b'x')
    query=query_path(d,'match-*')
    wanted={str(d/f'match-{i:04}').encode() for i in range(100)}
    deadline=time.monotonic()+a.freshness_s
    while True:
        expected=contents(query)
        if set(find_bytes(query).splitlines())==wanted:break
        assert time.monotonic()<deadline,(wanted-set(expected.splitlines()),expected)
        time.sleep(.2)
    fd=os.open(query,os.O_RDONLY)
    try:
        expected=os.read(fd,1<<20);os.lseek(fd,0,0)
        prefix=os.read(fd,17)
        (peer/'snapshot'/'match-new').write_bytes(b'y');time.sleep(5.2)
        # Fill distinct current query slots while this fd remains open.
        for i in range(20):contents(query_path(d,f'absent-{i:03}*'))
        rest=b''
        while b:=os.read(fd,31):rest+=b
        assert prefix+rest==expected,(len(prefix+rest),len(expected))
        assert os.fstat(fd).st_size==len(expected)
        os.lseek(fd,0,0);assert os.read(fd,len(expected)+100)==expected
    finally:os.close(fd)
    assert b'match-new' in find_bytes(query)
def stats():
    d=root/'stats';d.mkdir();(peer/'stats'/'payload').write_bytes(b'x'*4096)
    values=expect_stats(d,imm_files=1,imm_bytes=4096)
    assert int(values['imm_files'])==1,values
    assert int(values['imm_bytes'])==4096,values
def stats_values(d):
    return json.loads(contents(d/'.efs/.query/v1.stats'))['stats']
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
    matches=find_bytes(query_path(d,'payload'))
    assert b'moved/payload' in matches,matches
    os.unlink(pd/'alias');os.unlink(pd/'moved'/'payload');os.rmdir(pd/'moved')
    expect_stats(d,imm_files=0,imm_dirs=0,imm_bytes=0,tree_files=0,tree_dirs=0,tree_bytes=0)
def invalid():
    try:contents(root/'.efs/.query/v1.find.99.61')
    except OSError as e:assert e.errno==errno.EINVAL,e;return
    raise AssertionError('invalid query accepted')
def permissions():
    d=root/'protected';d.mkdir();(d/'secret-file').write_bytes(b'x');os.chmod(d,0o100)
    try:
        for q in [d/'.efs/.query/v1.stats',query_path(d,'secret-*')]:
            try:contents(q)
            except OSError as e:assert e.errno==errno.EACCES,e
            else:raise AssertionError('protected query succeeded: '+str(q))
    finally:os.chmod(d,0o700)
def nested_authorization():
    d=root/'authorization';d.mkdir();secret=d/'hidden';secret.mkdir()
    (secret/'secret-payload').write_bytes(b'classified')
    query=query_path(d,'secret-*')
    deadline=time.monotonic()+a.freshness_s
    while b'secret-payload' not in find_bytes(query):
        assert time.monotonic()<deadline,'index did not converge';time.sleep(.2)
    fd=os.open(query,os.O_RDONLY);expected=os.read(fd,1<<20);os.lseek(fd,0,0)
    os.chmod(secret,0)
    try:
        try:contents(query)
        except OSError as e:assert e.errno==errno.EACCES,e
        else:raise AssertionError('warm query leaked protected descendant')
        assert os.read(fd,1<<20)==expected,'already-open bytes changed on chmod'
    finally:os.chmod(secret,0o700);os.close(fd)
    group=d/'group';group.mkdir();(group/'group-secret').write_bytes(b'x');os.chmod(group,0o070)
    try:
        # Direct credential-boundary gate: unprivileged subject has only a
        # supplementary group grant. No root/group changes on the host.
        from query_rpc import request,query as rpc
        ports=os.environ.get('EFS_QUERY_TEST_PORT')
        if not ports:raise AssertionError('supplementary-group gate requires explicit private cohost port')
        st=os.stat(group);subject=st.st_uid+10000
        req=request(root=st.st_ino,uid=subject,gid=st.st_gid+10000,operation=1,term=b'group-secret')
        deadline=time.monotonic()+a.freshness_s
        while True:
            reply,text=rpc(int(ports),req)
            if reply.status!=-13:break
            assert time.monotonic()<deadline,'group directory was not published';time.sleep(.2)
        assert reply.status==-16,(reply.status,text)
        req.ngroups=1;req.groups[0]=st.st_gid
        deadline=time.monotonic()+a.freshness_s
        while True:
            reply,text=rpc(int(ports),req)
            if reply.status==0 and text.endswith(b'/group/group-secret\n'):break
            assert reply.status in (0,-13) and time.monotonic()<deadline,(reply.status,text)
            time.sleep(.2)
        assert text.endswith(b'/group/group-secret\n'),text
    finally:os.chmod(group,0o700)
def name_limits():
    d=root/'name-limits';d.mkdir();(d/'line\nbreak').write_bytes(b'x')
    deadline=time.monotonic()+a.freshness_s
    while True:
        result=json.loads(contents(query_path(d,'*line*')))
        if any(bytes.fromhex(e['path_hex'])==b'line\nbreak' for e in result['entries']):break
        assert time.monotonic()<deadline,'raw newline name absent';time.sleep(.2)
    deep=d/'deep';deep.mkdir();current=deep
    for _ in range(140):current=current/'x';current.mkdir()
    (current/'deep-payload').write_bytes(b'x')
    deadline=time.monotonic()+a.freshness_s
    while b'deep-payload' not in find_bytes(query_path(deep,'deep-payload')):
        assert time.monotonic()<deadline,'deep indexed query did not converge';time.sleep(.2)
def fd_capacity():
    d=root/'capacity';d.mkdir(exist_ok=True);(d/'capacity-file').write_bytes(b'x')
    q=query_path(d,'capacity-*');deadline=time.monotonic()+a.freshness_s
    while True:
        expected=contents(q)
        if [bytes.fromhex(e['path_hex']) for e in json.loads(expected)['entries']]==[b'capacity-file']:break
        assert time.monotonic()<deadline,'capacity fixture did not publish'
        time.sleep(.2)
    fds=[];bounded=False
    try:
        for _ in range(80):
            try:fds.append(os.open(query_path(d,f'absent-capacity-{_:03}*'),os.O_RDONLY))
            except OSError as e:
                assert e.errno==errno.ENOMEM,(len(fds),e)
                bounded=True;break
        assert bounded and len(fds)>=20,len(fds)
        for fd in fds:assert not json.loads(os.read(fd,1<<20))['entries']
    finally:
        for fd in fds:os.close(fd)
    assert json.loads(contents(q))['entries']==json.loads(expected)['entries']
def recovery():
    d=root/'recovery';d.mkdir()
    for i in range(256):
        q=query_path(d,f'absent-stat-{i:03}*')
        assert os.stat(q).st_size>0
    for i in range(128):
        assert not json.loads(contents(query_path(d,f'absent-open-{i:03}*')))['entries']
    # Exhaust and drain twice, then use a different directory/query version.
    fd_capacity();fd_capacity();fd_capacity_repeat=d/'repeat';fd_capacity_repeat.mkdir()
    assert not json.loads(contents(query_path(fd_capacity_repeat,'fresh-*')))['entries']
def retired_names():
    for old in ('.find','.stats'):
        path=root/old;path.write_bytes(b'ordinary user file')
        assert contents(path)==b'ordinary user file'
gate('retired_names',retired_names);gate('recovery',recovery);gate('snapshot',snapshot);gate('stats',stats);gate('stats_mutations',stats_mutations);gate('invalid',invalid);gate('permissions',permissions);gate('nested_authorization',nested_authorization);gate('name_limits',name_limits);gate('fd_capacity',fd_capacity)
print(json.dumps(dict(root=str(root),results=results)),flush=True)
raise SystemExit(any(r['status']!='PASS' for r in results))
