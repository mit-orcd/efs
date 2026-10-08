#!/usr/bin/env python3
"""New-directory, two-mount gates for snapshot fields, formatting and filters."""
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import time
from query_rpc import query as raw_query, request, filtered_request

def query(port, req):
    # Private fixture ports are consecutive by node id; the seed need not
    # host both metadata groups. Preserve business errors such as BUSY.
    base = int(os.environ['EFS_QUERY_TEST_PORT'])
    seen = set()
    for _ in range(4):
        assert port not in seen, ('query redirect loop', port, seen)
        seen.add(port)
        reply, text = raw_query(port, req)
        if reply.status != -15:
            return reply, text
        assert 1 <= reply.primary_id <= 4, (reply.status, reply.primary_id)
        port = base + reply.primary_id - 1
    raise AssertionError('query redirect bound exceeded')

mount, peer = map(Path, sys.argv[1:3])
root = mount / 'efs-options-gates'
other = peer / root.name
root.mkdir()
(root/'child').mkdir()
old_ns = 1577836800123456789
recent_ns = time.time_ns()-60*10**9
files = {'a-old.bin': (3, old_ns), 'b-recent.bin': (2048, recent_ns),
         'child/nested.bin': (7, old_ns+100)}
for i in range(8):
    files['extra-%d.bin'%i] = (i+10, old_ns+200+i)
files['line\nbreak.bin'] = (4, old_ns+300)
for name,(size,stamp) in files.items():
    (root/name).write_bytes(b'x'*size)
    os.utime(root/name,ns=(stamp,stamp))
(root/'alias').symlink_to('a-old.bin')

def cmd(command,*args,code=0):
    p=subprocess.run([sys.executable,str(other/'.efs'/command),*args],capture_output=True,timeout=30)
    assert p.returncode==code,(command,args,p.returncode,p.stderr)
    return p

def eventually(check):
    end=time.monotonic()+25
    while True:
        try:
            return check()
        except (AssertionError,OSError):
            if time.monotonic()>end: raise
            time.sleep(.2)

def get(*args):
    return json.loads(cmd('find','',*args,'--json').stdout)

def paths(result):
    return [bytes.fromhex(e['path_hex']).decode() for e in result['entries']]

def fields_gate():
    result=get('--type','file')
    assert set(paths(result))==set(files),result
    for entry in result['entries']:
        name=bytes.fromhex(entry['path_hex']).decode()
        live=os.lstat(root/name)
        assert (entry['size'],entry['mtime_ns'],entry['uid'],entry['gid'],entry['mode']) == (
            live.st_size,live.st_mtime_ns,live.st_uid,live.st_gid,live.st_mode),(name,entry,live)
        assert entry['expiry'] is None
    assert result['expiry']=='unavailable'
    return result
result=eventually(fields_gate)
assert paths(get('--type','dir'))==['child']
assert paths(get('--type','symlink'))==['alias']
assert paths(get('--type','file','--min-size','7','--max-size','7'))==['child/nested.bin']
assert paths(get('--type','file','--min-size','2KiB'))==['b-recent.bin']
assert paths(get('--type','file','--modified-within','2d'))==['b-recent.bin']
assert paths(get('--type','file','--modified-after','2020-01-01T00:00:00.123456789Z',
                 '--modified-before','2020-01-01T00:00:00.123456890Z'))==['child/nested.bin']
assert paths(get('--type','file','--max-size','3','--modified-before','2020-01-01T00:00:00.123456789Z'))==[]
print('server type/size/time filters AND together; ns boundaries and folded stat/owner fields PASS',flush=True)

listed=json.loads(cmd('ls','--json','--dirs-first','--sort','size','--reverse').stdout)
entries=listed['entries']
assert bytes.fromhex(entries[0]['path_hex'])==b'child'
assert [e['size'] for e in entries[1:]]==sorted(e['size'] for e in entries[1:])
long=cmd('find','a-old.bin','--match','exact','-l','--relative','--time-style','full').stdout
assert b'2020-01-01T00:00:00.123456789Z' in long and long.endswith(b'a-old.bin\n'),long
assert os.fsencode(other) not in long
assert b'2.0 KiB' in cmd('find','b-recent.bin','-l','--human-readable').stdout
assert b'ago' in cmd('find','b-recent.bin','-l','--time-style','relative').stdout
ordered=get('--type','file','--sort','mtime')
assert paths(ordered)[0]=='b-recent.bin'
assert paths(get('--type','file','--sort','mtime','-r'))[-1]=='b-recent.bin'
nul=cmd('find','nested','--type','file','-0','--relative').stdout
assert nul==b'child/nested.bin\0',nul
escaped=cmd('ls','-l').stdout
assert b'line\\u000abreak.bin' in escaped and b'line\nbreak.bin' not in escaped
print('sort directions/ties, directories first, UTC/relative dates, sizes, paths and control escaping PASS',flush=True)

port=int(os.environ['EFS_QUERY_TEST_PORT'])
ino=root.stat().st_ino
fields=dict(root=ino,match=3,term=b'',kind=1,flags=3,min_size=7,max_size=7,max_output=65536)
reply,text=query(port,filtered_request(**fields))
assert reply.status==0 and paths(json.loads(text))==['child/nested.bin'],(reply.status,text)
cap=len(text)+64
reply,text=query(port,filtered_request(**dict(fields,max_output=cap)))
assert reply.status==0,(reply.status,text)
reply,text=query(port,request(version=2,operation=1,root=ino,match=3,term=b'',max_output=cap))
assert reply.status==-22 and not text,(reply.status,len(text))
reply,text=query(port,filtered_request(root=ino,match=3,term=b'',flags=1,min_size=10**12,
                                     max_output=65536,visit_budget=1))
assert reply.status==-13 and not text,(reply.status,text)
print('filters precede output limits while candidate-visit bounds remain enforced PASS',flush=True)

# A filtered path has the same immutable-open and fresh-permission contract.
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'src/client/commands'))
from command import query_name
import errno
def open_query(path):
    for attempt in range(8):
        try: return open(path,'rb')
        except OSError as error:
            if error.errno not in (errno.ESTALE,errno.EAGAIN,errno.EBUSY) or attempt==7: raise
            time.sleep(.025*(attempt+1))
filters=dict(type='file',min_size=7,max_size=7,modified_after_ns=None,modified_before_ns=None)
qpath=root/'.efs/.query'/query_name('find',b'',filters=filters)
with open_query(qpath) as held:
    old=held.read();length=os.fstat(held.fileno()).st_size
    os.chmod(other/'child',0)
    try:
        denied=cmd('find','nested','--type','file','--min-size','7','--max-size','7',code=1)
        assert not denied.stdout and b'Permission denied' in denied.stderr,denied
        excluded=cmd('find','nested','--min-size','1TiB',code=1)
        assert not excluded.stdout and b'Permission denied' in excluded.stderr,excluded
        held.seek(0);assert held.read()==old and os.fstat(held.fileno()).st_size==length
    finally: os.chmod(other/'child',0o755)
    (other/'child/nested.bin').write_bytes(b'changed payload')
    def changed():
        assert paths(get('--type','file','--min-size','7','--max-size','7'))==[]
    eventually(changed)
    held.seek(0);assert held.read()==old
print('filtered result permission denial, immutable fd and peer size convergence PASS',flush=True)
for option,value in (('--expires-within','2d'),('--expires-before','2030-01-01T00:00:00Z')):
    p=cmd('find','',option,value,code=2)
    assert not p.stdout and b'unrecognized arguments' in p.stderr
assert b'Expiry: unavailable' in cmd('help','--extended').stdout
print('expiry unavailable and unsupported args refused PASS (no expiry deletion/renewal claim)',flush=True)
print('efs_command_options: PASS',flush=True)
