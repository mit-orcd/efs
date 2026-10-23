#!/usr/bin/env python3
"""Mount/control-owner proof on synthetic /proc fixtures, then actual Linux
process discovery and endpoint-owner retirement checks."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch
from contextlib import nullcontext
root=Path(__file__).resolve().parents[1]
script=root/'scripts/client_processes.py'
spec=importlib.util.spec_from_file_location('clients',script)
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
m.NATURAL_RETIRE_S=2  # the refusal cases wait out the full bound; keep it short

def refused(text,call,*args,**kw):
    try: call(*args,**kw)
    except RuntimeError as exc:
        assert text in str(exc),(text,str(exc));return
    raise AssertionError(f'expected refusal: {text}')

assert m.canonical('/a/../../b/./c//')=='/b/c' and m.canonical('x/../y','/m')=='/m/y'
assert m.endpoint('/mnt/a/../efs')==m.endpoint('/mnt/efs')
ME,OTHER=1000,2000
with tempfile.TemporaryDirectory(prefix='efs-processes-') as directory:
    proc=Path(directory)/'proc';tmp=str(Path(directory)/'tmp')
    (proc/'self').mkdir(parents=True);(proc/'net').mkdir();os.mkdir(tmp)
    mountinfo=[];unix=[];owners={}
    def mount(path,uid,fstype='fuse.efs-fuse',device='0:52'):
        mountinfo.append(f'{len(mountinfo)+30} 1 {device} / {path.replace(" ",chr(92)+"040")} rw,nosuid - {fstype} efs rw,user_id={uid},group_id={uid}')
        (proc/'self/mountinfo').write_text('\n'.join(mountinfo)+'\n')
    def listen(path,inode,uid=ME):
        unix.append(f'0000: 00000002 00000000 00010000 0001 01 {inode} {m.endpoint(path,tmp)}')
        (proc/'net/unix').write_text('Num RefCount Protocol Flags Type St Inode Path\n'+'\n'.join(unix)+'\n')
        owners[m.endpoint(path,tmp)]=uid
    def unlisten(*inodes):
        unix[:]=[l for l in unix if l.split()[6] not in map(str,inodes)]
        (proc/'net/unix').write_text('Num RefCount Protocol Flags Type St Inode Path\n'+'\n'.join(unix)+'\n')
    def process(pid,uid,fds=(),argv=b'',cwd='/',comm='efs-fuse',state='S'):
        p=proc/str(pid);p.mkdir();(p/'fd').mkdir()
        (p/'comm').write_text(comm+'\n');(p/'status').write_text(f'Name:\t{comm}\nUid:\t{uid}\t{uid}\t{uid}\t{uid}\n')
        (p/'stat').write_text(f'{pid} ({comm}) '+' '.join([state]+['0']*18+['99']))
        (p/'cmdline').write_bytes(argv);os.symlink(cwd,p/'cwd')
        for n,target in enumerate(fds): os.symlink(target,p/'fd'/str(n))
    (proc/'net/unix').write_text('Num RefCount Protocol Flags Type St Inode Path\n')
    # Foreign processes may be filtered by status only: argv, cwd and
    # descriptors of another uid are never read.
    foreign=set()
    real_readlink,real_iterdir,real_bytes,real_text=os.readlink,Path.iterdir,Path.read_bytes,Path.read_text
    def guard(path):
        parts=Path(path).relative_to(proc).parts if str(path).startswith(str(proc)) else ()
        if len(parts)>1 and parts[0] in foreign and parts[1] not in ('status',):
            raise AssertionError(f'foreign process inspected: {path}')
    guarded=[patch.object(os,'readlink',lambda p,*a,**k:(guard(p),real_readlink(p,*a,**k))[1]),
             patch.object(Path,'iterdir',lambda p:(guard(p),real_iterdir(p))[1]),
             patch.object(Path,'read_bytes',lambda p:(guard(p),real_bytes(p))[1]),
             patch.object(Path,'read_text',lambda p,*a,**k:(guard(p),real_text(p,*a,**k))[1]),
             patch.object(m,'socket_owner',lambda path:owners.get(path))]
    for g in guarded: g.start()
    run=lambda target:m.controller(target,proc,tmp,ME)
    # Own client on the target: found.
    mount('/mnt/own',ME);listen('/mnt/own',11)
    process(100,ME,['socket:[11]','/dev/fuse','/dev/null'])
    assert run('/mnt/own')==(100,'99') and run('/mnt/x/../own')==(100,'99')
    # Foreign client with a relative argv on an unrelated mount: ignored
    # without reading its cwd (the guard would raise).
    mount('/data1/efs/live/mnt',OTHER,device='0:60');listen('/data1/efs/live/mnt',21,OTHER)
    process(200,OTHER,['socket:[21]','/dev/fuse'],b'./efs-fuse\x00127.0.0.1:17432\x00fs\x00/data1/efs/live/mnt\x00',cwd='/data1/efs/live');foreign.add('200')
    assert run('/mnt/own')==(100,'99')
    assert m.clients(proc,tmp,ME)==[(100,'/mnt/own','99')]
    # Foreign client on the same target: refused.
    refused('mounted by uid 2000',run,'/data1/efs/live/mnt')
    # Detached foreign client on the same target (no mount left): its
    # endpoint names the owner; refused.
    listen('/mnt/gone',31,OTHER);process(201,OTHER,['socket:[31]','/dev/fuse']);foreign.add('201')
    refused('belongs to uid 2000',run,'/mnt/gone')
    # An endpoint held by nothing this user can inspect: refused.
    listen('/mnt/hidden',41,None);process(202,OTHER,['socket:[41]','/dev/fuse']);foreign.add('202')
    refused('held by no process',run,'/mnt/hidden')
    # Stale mount with no controller: no client.
    mount('/mnt/stale',ME,device='0:53')
    assert run('/mnt/stale') is None and run('/mnt/never') is None
    # Two candidate controllers: refused.
    mount('/mnt/two',ME,device='0:54');listen('/mnt/two',51)
    process(101,ME,['socket:[51]','/dev/fuse']);process(103,ME,['socket:[51]','/dev/fuse'])
    refused('ownership ambiguous',run,'/mnt/two')
    refused('ownership ambiguous',m.clients,proc,tmp,ME)
    mountinfo.pop();(proc/'self/mountinfo').write_text('\n'.join(mountinfo)+'\n')
    refused('ownership ambiguous',run,'/mnt/two')  # detached: still refused
    unlisten(51)
    # Endpoint held without a FUSE connection, or by another program: refused.
    mount('/mnt/nofuse',ME,device='0:55');listen('/mnt/nofuse',61)
    process(104,ME,['socket:[61]'])
    refused('without a FUSE connection',run,'/mnt/nofuse')
    mount('/mnt/impostor',ME,device='0:56');listen('/mnt/impostor',71)
    process(105,ME,['socket:[71]','/dev/fuse'],comm='python3')
    refused('without a FUSE connection',run,'/mnt/impostor')
    # Stacked efs-fuse mounts on one path: refused.
    mount('/mnt/own',ME,device='0:57')
    refused('stacked',run,'/mnt/own')
    for _ in range(3): mountinfo.pop()
    (proc/'self/mountinfo').write_text('\n'.join(mountinfo)+'\n');unlisten(61,71)
    # Detached own clients are named only by an argv entry whose endpoint
    # matches the one they hold, wrapped (memcheck) or relative.
    listen('/mnt/det',81)
    process(106,ME,['socket:[81]','/dev/fuse'],b'/usr/libexec/valgrind/memcheck-amd64-linux\x00--log-file=/tmp/c.vg\x00./efs-fuse\x00seed:1\x00det\x00-f\x00',cwd='/mnt')
    assert (106,'/mnt/det','99') in m.clients(proc,tmp,ME)
    (proc/'106/cmdline').write_bytes(b'./efs-fuse\x00seed:1\x00elsewhere\x00')
    refused('cannot name',m.clients,proc,tmp,ME)
    unlisten(81)
    assert m.clients(proc,tmp,ME)==[(100,'/mnt/own','99')]
    for g in guarded[:4]: g.stop()
    # Root inspects every uid; a detached client's endpoint names its owner.
    assert m.controller('/mnt/gone',proc,tmp,0)==(201,'99')
    # Identity: PID reuse and zombies are not the drained process.
    assert m.alive(100,'99',proc) and not m.alive(100,'98',proc) and not m.alive(999,'99',proc)
    (proc/'100/stat').write_text('100 (efs-fuse) '+' '.join(['Z']+['0']*18+['99']))
    assert not m.alive(100,'99',proc)
    guarded[4].stop()
print('client processes: synthetic mount/control-owner proof PASS')

if not sys.platform.startswith('linux'):
    print('NOTRUN: client processes: actual /proc checks need Linux');raise SystemExit(0)
# The real host: discovery must succeed for this user whatever other users'
# clients run here, and must refuse every efs-fuse mount of another uid.
listed=subprocess.run([sys.executable,str(script),'list'],capture_output=True,text=True)
assert listed.returncode==0,listed.stderr
for path,device,owner in m.mounts():
    if os.getuid()!=0 and owner!=os.getuid():
        refused(f'mounted by uid {owner}',m.controller,path)
        assert path not in listed.stdout.splitlines()
print(f'client processes: host discovery PASS ({len(m.mounts())} efs-fuse mounts visible)')
if not os.path.exists('/dev/fuse'):
    print('NOTRUN: client processes: /dev/fuse absent');raise SystemExit(0)
# Isolated synthetic daemon; never touch an actual cluster/client mount.
with tempfile.TemporaryDirectory(prefix='efs-process-owner-') as directory:
    mount=str(Path(directory)/'mount')
    endpoint=m.endpoint(mount)
    code='''import ctypes,os,socket,sys,time
ctypes.CDLL(None).prctl(15,b"efs-fuse",0,0,0)
if sys.argv[3]!="nofuse": fuse=os.open("/dev/fuse",os.O_RDWR)
s=socket.socket(socket.AF_UNIX);s.bind(sys.argv[2]);s.listen(4)
if sys.argv[3]=="closing":
 s.close();os.unlink(sys.argv[2])
print("READY",flush=True)
time.sleep(.5 if sys.argv[3]=="closing" else 30)
'''
    for case in ('owned','other','closing','denied','nofuse'):
        bound=endpoint+'.other' if case=='other' else endpoint
        process=subprocess.Popen([sys.executable,'-c',code,mount,bound,case],stdout=subprocess.PIPE,text=True)
        try:
            assert process.stdout.readline().strip()=='READY'
            stamp=m.stat_fields(Path('/proc'),process.pid)[19]
            original_iterdir=Path.iterdir
            def denied_fd(path):
                if path==Path(f'/proc/{process.pid}/fd'):raise PermissionError('simulated exit/access race')
                return original_iterdir(path)
            with patch.object(Path,'iterdir',denied_fd) if case=='denied' else nullcontext():
                if case=='owned':
                    assert m.controller(mount)==(process.pid,stamp)
                    assert (process.pid,mount,stamp) in m.clients()
                    m.retire(mount,process.pid,stamp)
                    assert process.wait(timeout=2)==-15
                elif case=='closing':
                    assert m.controller(mount) is None
                    m.retire(mount,process.pid,stamp)
                    assert process.wait(timeout=2)==0
                else:
                    if case=='other': assert m.controller(mount) is None
                    else: refused('held by no process' if case=='denied' else 'without a FUSE connection',m.controller,mount)
                    try: m.retire(mount,process.pid,stamp)
                    except RuntimeError: pass
                    else: raise AssertionError('unproven endpoint ownership must not authorize retirement')
                    assert process.poll() is None
        finally:
            if process.poll() is None: process.terminate();process.wait()
            try: os.unlink(bound)
            except FileNotFoundError: pass
print('client processes: endpoint-owner retirement and refusal PASS')
