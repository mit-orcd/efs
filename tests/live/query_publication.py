#!/usr/bin/env python3
"""Private TCP fixture: background query publication/startup/shutdown smoke.

Optional EFS_QUERY_RPC/FUSE/POSIX gates cover real query consumers; this is
not large-tree, hardware durability or deployment acceptance.
Keeps raw stores/logs in a caller-provided NEW directory. Never touches a
running cluster or mounts; terminates only the subprocesses it starts.
"""
import os
import argparse
from pathlib import Path
import re
import socket
import subprocess
import sys
import time

source=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('work',type=Path)
parser.add_argument('--mode',choices=('direct','buffered'),default='direct')
parser.add_argument('--mount',type=Path)
parser.add_argument('--peer-mount',type=Path)
args=parser.parse_args()
assert bool(args.mount)==bool(args.peer_mount),'both mount paths required together'
work=args.work.resolve()
mount_paths=[args.mount,args.peer_mount] if args.mount else [work/'mnt1',work/'mnt2']
for mount in mount_paths:
    if mount.exists():
        assert mount.is_dir() and not os.path.ismount(mount) and not any(mount.iterdir()),'mount must be empty and unmounted'
io_flag='--direct-io' if args.mode=='direct' else '--no-direct-io'
work.mkdir(parents=True,exist_ok=False)
logs=[];procs=[];fuse_procs=[];mounts=[];held_stream=None
env=dict(os.environ,EFS_MD_RAFT_N='4',EFS_TRANSPORT='tcp')
if os.environ.get('EFS_QUERY_FUSE')=='1': env['EFS_COMMANDS']='1'
# Reserve consecutive candidates while checking; no unrelated process is killed.
for base in range(24800,25000,4):
    held=[]
    try:
        for port in range(base,base+4):
            sock=socket.socket();held.append(sock);sock.bind(('127.0.0.1',port))
        break
    except OSError:
        continue
    finally:
        for sock in held:sock.close()
else:
    raise RuntimeError('no free fixture port range')
print(f'fixture={work} ports={base}-{base+3} mode={args.mode} transport=tcp',flush=True)
def stop_mount(mount):
    if not os.path.ismount(mount):return
    # Drain while our servers are still alive. Foreground FUSE below keeps
    # Popen attached to the actual client, so wait proves teardown completed.
    with (work/'client-stop.log').open('a') as output:
        subprocess.run(['bash',str(source/'scripts/client.sh'),'stop',str(mount)],
                       cwd=source,env=env,stdout=output,stderr=subprocess.STDOUT,
                       check=True,timeout=90)
def run(*args):
    return subprocess.run([str(source/'efs-mgmt'),*map(str,args)],env=env,text=True,
                          stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=8)
def wait_for(fn,seconds=25):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        assert all(p.poll() is None for p in procs),'fixture daemon exited'
        if fn():return
        time.sleep(.1)
    raise AssertionError('timed out waiting for fixture condition')
def published(node,files):
    text=(work/f'n{node}.log').read_text()
    return any(re.search(rf'root_rc=0 files={files} dirs=0 logical=0(?:\s|$)',line)
               for line in text.splitlines() if 'query-publish:' in line)
try:
    for node in range(1,5):
        log=(work/f'n{node}.log').open('w');logs.append(log)
        args=[str(source/'efsd'),'--node-id',str(node),'--addr','127.0.0.1',
              '--port',str(base+node-1),'--storage',str(work/f'n{node}'),
              '--quota','1G','--writers','0',io_flag]
        if node>1:args+=['--join',f'127.0.0.1:{base}']
        procs.append(subprocess.Popen(args,env=env,stdout=log,stderr=log))
        wait_for(lambda: 'listening' in (work/f'n{node}.log').read_text(),10)
    def mkfs():
        for node in range(4):
            result=run('raft-mkfs',f'127.0.0.1:{base+node}')
            print(result.stdout.strip(),flush=True)
            if result.returncode==0:return True
        return False
    wait_for(mkfs)
    wait_for(lambda:published(2,0) and published(3,0))
    print('initial complete-group catalogs PASS',flush=True)
    # Parent root=1; actual Raft mutations, no test-only catalog injection.
    for index in range(4):
        def create():
            for node in range(4):
                result=run('raft-create',f'127.0.0.1:{base+node}',1,f'file-{index}')
                print(result.stdout.strip(),flush=True)
                if result.returncode==0:return True
            return False
        wait_for(create)
    wait_for(lambda:published(2,4) and published(3,4))
    assert not any('query-publish:' in (work/f'n{node}.log').read_text() for node in (1,4)), 'partial hosts published namespace'
    print('quiet metadata refresh and partial-host refusal PASS',flush=True)
    if os.environ.get('EFS_QUERY_RPC')=='1':
        from query_rpc import request,query,filtered_request
        reply,text=query(base+1,request())
        assert reply.status==0,(reply.status,text)
        assert b'tree_files=4\n' in text and reply.checked_dirs==1,text
        generation=reply.generation;applied=list(reply.applied)
        reply,text=query(base,request())
        assert reply.status==-15 and reply.primary_id in (2,3),(reply.status,reply.primary_id)
        for _ in range(12):
            reply,text=query(base+1,request(operation=1,match=1,term=b'file-'))
            assert reply.status==0 and set(text.splitlines())=={f'file-{i}'.encode() for i in range(4)},(reply.status,text)
            assert reply.generation==generation and list(reply.applied)==applied,'query rebuilt catalog'
        for fields,status in [({'version':4},-5),({'version':3},-5),({'ngroups':129},-5),({'max_output':1},-22),
                              ({'operation':1,'match':1,'term':b'file-','visit_budget':1},-13),
                              ({'operation':1,'term':b'bad*term'},-5),({'root':999999},-3)]:
            reply,text=query(base+1,request(**fields));assert reply.status==status,(fields,reply.status,status)
        reply,text=query(base+1,request(),raw=b'bad')
        assert reply.status==-5,reply.status
        print('v1 RPC stats/find, unchanged publication, redirects and explicit errors PASS',flush=True)
        import json
        sys.path.insert(0,str(source/'src/client/commands'))
        from command import validate_result
        for operation,command in ((0,'stats'),(1,'find'),(2,'ls')):
            fields=dict(version=2,operation=operation,max_output=65536)
            if operation==1: fields.update(match=3,term=b'file-')
            reply,text=query(base+1,request(**fields))
            assert reply.status==0,(command,reply.status,text)
            result=json.loads(text)
            paths=validate_result(result,command)
            if command=='stats': assert result['stats']['tree_files']==4,result
            else: assert set(paths)=={f'file-{i}'.encode() for i in range(4)},result
            assert result['snapshot']['generation']==generation,'command rebuilt catalog'
            assert result['expiry']=='unavailable',result
        for fields in (dict(version=2,operation=2,max_output=1),
                       dict(version=2,operation=2,visit_budget=1),
                       dict(version=2,operation=1,match=3,term=b'x'*121)):
            reply,text=query(base+1,request(**fields))
            assert reply.status and not text,('command limit delivered partial success',fields,reply.status,text)
        print('v2 command JSON stats/find/ls, immutable catalog identity and explicit limits PASS',flush=True)
        for fields,status in ((dict(kind=1,flags=3,min_size=0,max_size=0),0),
                              (dict(kind=2),0),(dict(kind=4),-5),(dict(flags=16),-5),
                              (dict(min_size=1),-5),(dict(flags=3,min_size=2,max_size=1),-5),
                              (dict(flags=12,after_ns=2,before_ns=1),-5),
                              (dict(operation=0),-5),(dict(max_output=1),-22)):
            reply,text=query(base+1,filtered_request(match=1,term=b'file-',max_output=fields.get('max_output',65536),
                              **{k:v for k,v in fields.items() if k!='max_output'}))
            assert reply.status==status,(fields,reply.status,status)
            if not status:
                result=json.loads(text)
                assert len(result['entries'])==(0 if fields.get('kind')==2 else 4),result
        print('v3 type/size predicates, canonical fields and explicit limits PASS',flush=True)


    expected_root='root_rc=0 files=4 dirs=0 logical=0'
    if os.environ.get('EFS_QUERY_FUSE')=='1':
        for index in (1,2):
            mount=mount_paths[index-1].resolve();mount.mkdir(exist_ok=True);mounts.append(mount)
            log=(work/f'fuse{index}.log').open('w');logs.append(log)
            fuse_procs.append(subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{base}',
                                                'w92-private',str(mount),'-f'],env=env,stdout=log,stderr=log))
            wait_for(lambda:os.path.ismount(mount),20)
        env['EFS_QUERY_TEST_PORT']=str(base+1)
        if os.environ.get('EFS_QUERY_LARGE_DELTA')=='1':
            with (work/'large-delta-gates.log').open('w') as output:
                result=subprocess.run([sys.executable,str(source/'tests/live/query_large_delta.py'),
                                       *map(str,mounts),'--files',os.environ.get('EFS_QUERY_LARGE_DELTA_FILES','1025'),*(['--metadata-only'] if os.environ.get('EFS_QUERY_LARGE_DELTA_MODE','metadata')=='metadata' else [])],env=env,stdout=output,stderr=subprocess.STDOUT,timeout=300)
            assert result.returncode==0,(work/'large-delta-gates.log').read_text()
            print('FUSE/query mutation and independent peer oracle gates PASS',flush=True)

        with (work/'virtual-gates.log').open('w') as output:
            result=subprocess.run([sys.executable,str(source/'tests/live/efs_commands.py'),
                                   *map(str,mounts)],env=env,stdout=output,stderr=subprocess.STDOUT,timeout=240)
        assert result.returncode==0,(work/'virtual-gates.log').read_text()
        print('two-mount virtual interface gates PASS',flush=True)
        with (work/'options-gates.log').open('w') as output:
            result=subprocess.run([sys.executable,str(source/'tests/live/efs_command_options.py'),
                                   *map(str,mounts)],env=env,stdout=output,stderr=subprocess.STDOUT,timeout=180)
        assert result.returncode==0,(work/'options-gates.log').read_text()
        print('two-mount command display, sorting, server filters and timestamp oracles PASS',flush=True)
        with (work/'page-gates.log').open('w') as output:
            result=subprocess.run([sys.executable,str(source/'tests/live/efs_command_pages.py'),
                                   *map(str,mounts),str(work)],env=env,stdout=output,stderr=subprocess.STDOUT,timeout=240)
        assert result.returncode==0,(work/'page-gates.log').read_text()
        print('bounded display, streaming export, pinned generation and permissions PASS',flush=True)
        if os.environ.get('EFS_QUERY_VIRTUAL')=='1':
            with (work/'virtual-regression-gates.log').open('w') as output:
                result=subprocess.run([sys.executable,str(source/'tests/live/virtual_interfaces.py'),
                                       *map(str,mounts)],env=env,stdout=output,stderr=subprocess.STDOUT,timeout=360)
            assert result.returncode==0,(work/'virtual-regression-gates.log').read_text()
            print('current query snapshots, permission, raw names and FD recovery PASS',flush=True)



        if os.environ.get('EFS_QUERY_EXTRAS')=='1':
            results=work/'posix-extras.tsv'
            with (work/'posix-extras.log').open('w') as output:
                result=subprocess.run([sys.executable,str(source/'tests/posix/posix_extras.py'),
                                       str(mounts[0]),'--jobs','4','--timeout-s','30',
                                       '--results',str(results)],env=env,stdout=output,
                                      stderr=subprocess.STDOUT,timeout=180)
            assert result.returncode==0,(work/'posix-extras.log').read_text()
            verdicts=dict(line.split('\t',2)[:2] for line in results.read_text().splitlines()
                          if line and not line.startswith('#'))
            for name in ('virt_stats_readable','virt_find_query','virt_find_not_a_real_dir'):
                assert verdicts.get(name)=='PASS',(name,verdicts)
            print('optional POSIX suite: enabled .efs stats/find/source contracts PASS; mmap verdict retained',flush=True)

        if os.environ.get('EFS_QUERY_POSIX')=='1':
            suites=[('posix',[str(source/'tests/posix/posix_suite.py'),str(mounts[0]),'--results',str(work/'posix.tsv')]),
                    ('peer',[str(source/'tests/posix/posix_2client.py'),'--local',*map(str,mounts),'--results',str(work/'peer.tsv')])]
            for label,args in suites:
                with (work/f'{label}.log').open('w') as output:
                    result=subprocess.run([sys.executable,*args],env=env,stdout=output,stderr=subprocess.STDOUT,timeout=360)
                assert result.returncode==0,(work/f'{label}.log').read_text()
                print(label+' full suite PASS',flush=True)

        if os.environ.get('EFS_QUERY_FAILOVER')=='1':
            # Remove only our own fixture's n2 (full-group publisher; possibly leader).
            lost=procs.pop(1);lost.terminate();lost.wait(15)
            marker=mounts[1]/'failover-marker'
            marker.write_bytes(b'leader change payload')
            recovery_deadline=time.monotonic()+45
            def failover_query():
                remaining=recovery_deadline-time.monotonic()
                if remaining<=0:return False
                try:
                    result=subprocess.run([sys.executable,str(mounts[0]/'.efs/find'),
                                           'failover-marker','--match','exact','--json'],
                                          env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                          timeout=min(35,remaining))
                except subprocess.TimeoutExpired as error:
                    with (work/'failover-attempts.log').open('a') as output:
                        output.write('query attempt timed out within recovery budget: '+repr(error)+'\n')
                    return False
                with (work/'failover-attempts.log').open('ab') as output:
                    output.write(result.stderr)
                if result.returncode:return False
                entries=json.loads(result.stdout)['entries']
                return any(bytes.fromhex(e['path_hex'])==b'failover-marker' for e in entries)
            wait_for(failover_query,45)
            log=(work/'n2-rejoin.log').open('w');logs.append(log)
            args=[str(source/'efsd'),'--node-id','2','--addr','127.0.0.1',
                  '--port',str(base+1),'--storage',str(work/'n2'),'--quota','1G',
                  '--writers','0',io_flag,'--join',f'127.0.0.1:{base}']
            procs.append(subprocess.Popen(args,env=env,stdout=log,stderr=log))
            wait_for(lambda:'listening' in (work/'n2-rejoin.log').read_text(),20)
            recovery_deadline=time.monotonic()+30
            wait_for(failover_query,30)
            print('full-group publisher loss, peer query and persisted rejoin PASS',flush=True)
        if os.environ.get('EFS_QUERY_LOAD')=='1':
            with (work/'loaded-gates.log').open('w') as output:
                result=subprocess.run([sys.executable,str(source/'tests/live/efs_commands_load.py'),
                                       *map(str,mounts)],env=env,stdout=output,stderr=subprocess.STDOUT,timeout=150)
            assert result.returncode==0,(work/'loaded-gates.log').read_text()
            print('loaded publication and payload oracle PASS',flush=True)
        if os.environ.get('EFS_QUERY_NOEXEC')=='1':
            noexec=work/'noexec';noexec.mkdir();mounts.append(noexec)
            log=(work/'noexec.log').open('w');logs.append(log)
            proc=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{base}',
                                   'w92-private',str(noexec),'-f','-o','noexec'],env=env,stdout=log,stderr=log)
            fuse_procs.append(proc)
            wait_for(lambda:os.path.ismount(noexec),20)
            try:
                subprocess.run([str(noexec/'.efs/help')],check=True,timeout=10)
            except PermissionError:
                pass
            else:
                raise AssertionError('noexec allowed direct command execution')
            result=subprocess.run([sys.executable,str(noexec/'.efs/stats'),'--json'],
                                  check=True,stdout=subprocess.PIPE,timeout=15)
            assert json.loads(result.stdout)['complete'] is True
            print('noexec refuses execution; explicit interpreter PASS',flush=True)

        if os.environ.get('EFS_QUERY_NOEXEC')=='1':
            disabled=work/'disabled';disabled.mkdir();mounts.append(disabled)
            log=(work/'disabled.log').open('w');logs.append(log)
            off_env=dict(env,EFS_COMMANDS='0')
            proc=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{base}',
                                   'w92-private',str(disabled),'-f'],env=off_env,stdout=log,stderr=log)
            fuse_procs.append(proc)
            wait_for(lambda:os.path.ismount(disabled),20)
            assert not os.path.lexists(disabled/'.efs'),'disabled mount exposed synthetic commands'
            real=list(disabled.glob('efs-command-gates-*/collision/.efs'))
            assert real and real[0].is_file(),'disabled mount hid real user .efs'
            print('cold export opt-out and real .efs precedence PASS',flush=True)

        # Independent syscall oracle after workloads close; this test-only
        # walk is never part of a virtual query or server publication path.
        total_files=total_dirs=total_bytes=0
        for directory,dirs,files in os.walk(mounts[0]):
            # Synthetic .efs is lookup-only and never returned by READDIR.
            # Real .efs/.find/.stats entries must remain in this oracle.
            total_dirs+=len(dirs);total_files+=len(files)
            total_bytes+=sum(os.lstat(Path(directory)/name).st_size for name in files)
        expected_root=f'root_rc=0 files={total_files} dirs={total_dirs} logical={total_bytes}'
        print('restart oracle '+expected_root,flush=True)
        if os.environ.get('EFS_QUERY_POLICY')=='1':
            for attempt in range(8):
                try:
                    held_stream=open(mounts[0]/'.efs/.query/v1.stats','rb')
                    break
                except OSError:
                    if attempt==7: raise
                    time.sleep(.1)
            held_bytes=held_stream.read();held_size=os.fstat(held_stream.fileno()).st_size
        else:
            for mount in mounts:stop_mount(mount)
            for proc in fuse_procs:proc.wait(10);assert proc.returncode==0,proc.returncode
            fuse_procs.clear();mounts.clear()
    for proc in procs:proc.terminate()
    for proc in procs:
        proc.wait(8);assert proc.returncode==0,'first fixture shutdown'
    procs.clear()
    if os.environ.get('EFS_QUERY_POLICY')=='1':
        disabled_env=dict(env,EFS_COMMANDS_SERVER='0')
        for node in range(1,5):
            log=(work/f'n{node}-off.log').open('w');logs.append(log)
            args=[str(source/'efsd'),'--node-id',str(node),'--addr','127.0.0.1',
                  '--port',str(base+node-1),'--storage',str(work/f'n{node}'),
                  '--quota','1G','--writers','0',io_flag]
            if node>1:args+=['--join',f'127.0.0.1:{base}']
            procs.append(subprocess.Popen(args,env=disabled_env,stdout=log,stderr=log))
            wait_for(lambda:'listening' in (work/f'n{node}-off.log').read_text(),10)
        from query_rpc import request,query,filtered_request,features
        for node in range(4):
            assert features(base+node)==0,('disabled feature mask',node)
            for version in (1,2,3):
                reply,text=query(base+node,filtered_request() if version==3 else request(version=version))
                assert reply.status==-3 and not text,('disabled RPC succeeded',node,version,reply.status)
        offmount=work/'server-off';offmount.mkdir();mounts.append(offmount)
        log=(work/'server-off-fuse.log').open('w');logs.append(log)
        proc=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{base}',
                               'w92-private',str(offmount),'-f'],env=env,stdout=log,stderr=log)
        fuse_procs.append(proc)
        wait_for(lambda:os.path.ismount(offmount),20)
        assert not os.path.lexists(offmount/'.efs'),'server opt-out did not hide client commands'
        wait_for(lambda:not os.path.lexists(mounts[0]/'.efs'),10)
        held_stream.seek(0)
        assert held_stream.read()==held_bytes,'disablement rewrote delivered snapshot bytes'
        assert os.fstat(held_stream.fileno()).st_size==held_size
        held_stream.close();held_stream=None
        print('existing mount hides commands; authorized old descriptor retains immutable bytes PASS',flush=True)
        changed=offmount/'while-disabled';changed.mkdir()
        (changed/'gone').write_bytes(b'deleted during disabled interval')
        (changed/'gone').unlink()
        (changed/'old').write_bytes(b'current payload after disabled interval')
        (changed/'old').rename(changed/'renamed')
        (changed/'sub').mkdir()
        (changed/'sub/new').write_bytes(b'new payload')
        total_files=total_dirs=total_bytes=0
        for directory,dirs,files in os.walk(offmount):
            total_dirs+=len(dirs);total_files+=len(files)
            total_bytes+=sum(os.lstat(Path(directory)/name).st_size for name in files)
        expected_root=f'root_rc=0 files={total_files} dirs={total_dirs} logical={total_bytes}'
        for mount in mounts:stop_mount(mount)
        for proc in fuse_procs:proc.wait(10);assert proc.returncode==0
        fuse_procs.clear();mounts.clear()
        for node in range(1,5):
            text=(work/f'n{node}-off.log').read_text()
            assert 'no catalog worker or query tracking' in text
            assert 'query-publish:' not in text and 'query-refresh:' not in text
        for proc in procs:proc.terminate()
        for proc in procs:proc.wait(10);assert proc.returncode==0
        procs.clear()
        print('server-off: all three RPC versions refused, client hidden, mutations persisted, no catalog publications PASS',flush=True)
    env['EFS_COMMANDS_SERVER']='1'
    for node in range(1,5):
        log=(work/f'n{node}-restart.log').open('w');logs.append(log)
        args=[str(source/'efsd'),'--node-id',str(node),'--addr','127.0.0.1',
              '--port',str(base+node-1),'--storage',str(work/f'n{node}'),
              '--quota','1G','--writers','0',io_flag]
        if node>1:args+=['--join',f'127.0.0.1:{base}']
        procs.append(subprocess.Popen(args,env=env,stdout=log,stderr=log))
        wait_for(lambda: 'listening' in (work/f'n{node}-restart.log').read_text(),10)
    def recovered(node):
        lines=(work/f'n{node}-restart.log').read_text().splitlines()
        restored=any('query-restore: rc=0 backend=disk' in line for line in lines)
        return any('generation=1 ' in line and expected_root in line and
                   ('rebuilt=4096 ' in line or restored)
                   for line in lines if 'query-publish:' in line)
    wait_for(lambda:recovered(2) and recovered(3))
    print('restart catalog restore/rebuild from persisted metadata PASS',flush=True)
    if os.environ.get('EFS_QUERY_RPC')=='1' and os.environ.get('EFS_QUERY_FUSE')!='1':
        def recovered_names(node):
            reply,text=query(base+node-1,request(operation=1,match=1,term=b'file-'))
            if reply.status in (-8,-13):return False
            assert reply.status==0,(node,reply.status,text)
            assert set(text.splitlines())=={f'file-{i}'.encode() for i in range(4)},text
            return True
        for node in (2,3):wait_for(lambda node=node:recovered_names(node))
        print('restart RPC filenames match independent CREATE oracle PASS',flush=True)
    if os.environ.get('EFS_QUERY_POLICY')=='1':
        import json
        for node in (2,3):
            assert features(base+node-1)==3,('reenabled feature mask',node)
            # LOAD/POSIX gates may leave thousands of paths. Use the normal
            # bounded command output cap for this namespace-wide oracle;
            # 64 KiB deliberately remains in the small explicit-limit gates.
            if os.environ.get('EFS_QUERY_VIRTUAL')=='1' and os.getuid()!=1000:
                denied,body=query(base+node-1,request(version=2,max_output=16<<20,
                                                     operation=1,match=1,term=b''))
                assert denied.status==-16 and not body,('outsider read protected tree',denied.status,body)
            reply,text=query(base+node-1,request(version=2,max_output=16<<20,operation=1,
                                              uid=os.getuid(),gid=os.getgid(),match=1,term=b''))
            assert reply.status==0,(reply.status,text)
            paths={bytes.fromhex(e['path_hex']) for e in json.loads(text)['entries']}
            assert b'while-disabled/renamed' in paths and b'while-disabled/sub/new' in paths,paths
            assert b'while-disabled/gone' not in paths and b'while-disabled/old' not in paths,paths
        print('on/off/on: full persisted rebuild, exact stats oracle and current renamed/new/deleted paths PASS',flush=True)

finally:
    if held_stream:held_stream.close()
    # If clean drain is refused, retain the private clients AND servers for
    # recovery instead of killing a client with unresolved writes.
    for mount in mounts:stop_mount(mount)
    for proc in fuse_procs:
        if proc.poll() is None:proc.terminate()
        try:proc.wait(10)
        except subprocess.TimeoutExpired:
            raise AssertionError('fixture client still draining; servers retained')
    for proc in procs:
        if proc.poll() is None:proc.terminate()
    for proc in procs:
        try:proc.wait(8)
        except subprocess.TimeoutExpired:
            proc.kill();proc.wait()
            raise AssertionError('fixture shutdown timeout; killed owned process')
        assert proc.returncode==0,f'fixture daemon exit {proc.returncode}'
    for log in logs:log.close()
print('query_publication: PASS (smoke only)',flush=True)
