#!/usr/bin/env python3
"""Current .efs query gates (v3. page names); retired .find/.stats remain ordinary user names.

Every first page holds a server page session (16 per host) until closed;
reads here close it, so client-side capacity gates are not hidden by it."""
import argparse,errno,json,os,time,traceback
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from query_rpc import close_query,query_name
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
    match = ('exact', 'prefix', 'suffix', 'contains')[mode]
    nonce = int.from_bytes(os.urandom(4), 'big')
    path = directory / '.efs/.query' / query_name('find', raw, match, nonce=nonce)
    pages[str(path)] = ('find', raw, match, nonce)
    return path
pages = {}
def stats_path(directory):
    nonce = int.from_bytes(os.urandom(4), 'big')
    path = directory / '.efs/.query' / query_name('stats', nonce=nonce)
    pages[str(path)] = ('stats', b'', 'contains', nonce)
    return path
def release(path, data):
    if str(path) in pages:
        command, term, match, nonce = pages[str(path)]
        close_query(str(path.parent.parent.parent), command, data, term, match, None, nonce)
def until_indexed(fn):
    # Same bounded wait as contents(): ENODATA until the next refresh.
    indexed_by = time.monotonic() + a.freshness_s
    while True:
        try:
            return fn()
        except OSError as error:
            if error.errno != errno.ENODATA or time.monotonic() >= indexed_by:
                raise
            time.sleep(1)
def held_fd(path):
    fd = until_indexed(lambda: os.open(path, os.O_RDONLY))
    release(path, os.pread(fd, 1 << 20, 0))
    return fd
def find_bytes(path):
    value = json.loads(contents(path))
    assert value['complete'], value
    directory = path.parent.parent.parent
    return b''.join(os.fsencode(directory) + b'/' + bytes.fromhex(e['path_hex']) + b'\n' for e in value['entries'])
def contents(path):
    # ENODATA: the directory is not in the published index yet. A fresh
    # gate directory waits for the next refresh, bounded by --freshness-s.
    indexed_by=time.monotonic()+a.freshness_s
    attempt=0
    while True:
        try:
            with open(path,'rb',buffering=0) as f:data=f.read()
            release(path,data);return data
        except OSError as error:
            if error.errno==errno.ENODATA and time.monotonic()<indexed_by:
                time.sleep(1);continue
            if error.errno not in (errno.ESTALE,errno.EAGAIN,errno.EBUSY) or attempt==7:raise
            attempt+=1;time.sleep(.025*attempt)
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
    fd=held_fd(query)
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
    # A fresh query (new page identity) sees the peer's file once the index
    # refreshes; the held descriptor above kept its immutable bytes.
    deadline=time.monotonic()+a.freshness_s
    while b'match-new' not in find_bytes(query_path(d,'match-*')):
        assert time.monotonic()<deadline,'peer file never published';time.sleep(1)
def stats():
    d=root/'stats';d.mkdir();(peer/'stats'/'payload').write_bytes(b'x'*4096)
    values=expect_stats(d,imm_files=1,imm_bytes=4096)
    assert int(values['imm_files'])==1,values
    assert int(values['imm_bytes'])==4096,values
def stats_values(d):
    return json.loads(contents(stats_path(d)))['stats']
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
    # Removed unpaged names are refused like any malformed query name.
    for old in ('v1.find.0.61','v1.stats','v2.stats'):
        try:contents(root/'.efs/.query'/old)
        except OSError as e:assert e.errno==errno.EINVAL,e
        else:raise AssertionError('removed query name accepted: '+old)
    try:contents(root/'.efs/.query/v3.GG')
    except OSError as e:assert e.errno==errno.EINVAL,e;return
    raise AssertionError('invalid query accepted')
def permissions():
    d=root/'protected';d.mkdir();(d/'secret-file').write_bytes(b'x');os.chmod(d,0o100)
    try:
        for q in [stats_path(d),query_path(d,'secret-*')]:
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
        from query_rpc import page_request,routed_query
        ports=os.environ.get('EFS_QUERY_TEST_PORT')
        if not ports:raise AssertionError('supplementary-group gate requires explicit private cohost port')
        st=os.stat(group);subject=st.st_uid+10000
        fields=dict(root=st.st_ino,uid=subject,gid=st.st_gid+10000,operation=1,term=b'group-secret',limit=1000)
        def rpc(port,req):
            # One first page, closed at once; text is the matched paths.
            reply,text=routed_query(port,req)
            if reply.status or not text:return reply,text
            value=json.loads(text);page=value['page']
            if int(page['token']):
                close=page_request(**{k:v for k,v in fields.items() if k!='limit'},token=int(page['token']),cursor=int(page['cursor']),limit=0)
                close.ngroups=req.ngroups;close.groups[0]=req.groups[0]
                routed_query(port,close)
            return reply,b''.join(b'/'+bytes.fromhex(e['path_hex'])+b'\n' for e in value['entries'])
        req=page_request(**fields)
        deadline=time.monotonic()+a.freshness_s
        while True:
            reply,text=rpc(int(ports),req)
            if reply.status not in (-13,-21):break  # busy or not indexed yet
            assert time.monotonic()<deadline,'group directory was not published';time.sleep(.2)
        assert reply.status==-16,(reply.status,text)
        req.ngroups=1;req.groups[0]=st.st_gid
        deadline=time.monotonic()+a.freshness_s
        while True:
            reply,text=rpc(int(ports),req)
            if reply.status==0 and text==b'/group-secret\n':break  # page paths are root-relative
            assert reply.status in (0,-13,-21) and time.monotonic()<deadline,(reply.status,text)
            time.sleep(.2)
        assert text==b'/group-secret\n',text
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
    d=root/'capacity';d.mkdir(exist_ok=True)
    payload=d/'capacity-file'
    # Repeated exhaustion checks must leave the comparison fixture immutable.
    if not payload.exists():payload.write_bytes(b'x')
    q=query_path(d,'capacity-*');deadline=time.monotonic()+a.freshness_s
    while True:
        expected=contents(q)
        entries=json.loads(expected)['entries']
        if len(entries)==1 and bytes.fromhex(entries[0]['path_hex'])==b'capacity-file' and entries[0]['size']==1:break
        assert time.monotonic()<deadline,'capacity fixture did not publish'
        time.sleep(.2)
    fds=[];bounded=False
    try:
        for _ in range(80):
            try:fds.append(held_fd(query_path(d,f'absent-capacity-{_:03}*')))
            except OSError as e:
                assert e.errno==errno.ENOMEM,(len(fds),e)
                bounded=True;break
        assert bounded and len(fds)>=20,len(fds)
        for fd in fds:assert not json.loads(os.read(fd,1<<20))['entries']
    finally:
        for fd in fds:os.close(fd)
    actual=json.loads(contents(q))['entries'];wanted=json.loads(expected)['entries']
    assert actual==wanted,dict(expected=wanted,actual=actual)
def recovery():
    d=root/'recovery';d.mkdir()
    for i in range(256):
        q=query_path(d,f'absent-stat-{i:03}*')
        assert until_indexed(lambda:os.stat(q)).st_size>0
        contents(q)  # releases the page session the lookup created
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
