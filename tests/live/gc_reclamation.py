#!/usr/bin/env python3
"""Isolated four-node physical reclamation gate (Linux/FUSE; no existing stores).

Run on the NUC: python3 tests/live/gc_reclamation.py --mode direct
The two root parents must already exist. Only unique harness-owned directories
and processes are used; failed fixtures/logs are retained for diagnosis.
"""
import argparse
import ctypes
import concurrent.futures
import struct
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import threading
import socket
import subprocess
import tempfile
from raft_tail import commands as raft_commands
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--mode', choices=('buffered', 'direct'), required=True)
p.add_argument('--root', action='append', default=[])
p.add_argument('--posix', action='store_true')
p.add_argument('--integrity', action='store_true')
p.add_argument('--namespace', action='store_true')
p.add_argument('--namespace-boundary', action='store_true')
p.add_argument('--namespace-cycles',action='store_true',help='race opposite directory moves using captured inode parents')
p.add_argument('--mount-label', default='default')
p.add_argument('--workers', action='store_true')
p.add_argument('--pressure', action='store_true')
p.add_argument('--production-budgets',action='store_true',help='exercise default 256 MiB body and 64 MiB drain budgets')
p.add_argument('--drain-pressure',action='store_true',help='physically exhaust recovery scratch while publication is withheld')
p.add_argument('--memory-limit-bytes',type=int,help='require an inherited cgroup memory ceiling and verify no OOM events')
p.add_argument('--publication-pressure',action='store_true',help='retain failed REPORT bodies until bounded admission refuses, then recover')
p.add_argument('--namespace-max',action='store_true',help='exact ancestry limit, over-limit refusal and restart recovery')
p.add_argument('--opid', action='store_true')
p.add_argument('--opid-restart',action='store_true',help='restart all owned servers between lost commit reply and replay')
p.add_argument('--ior-binary',type=Path,help='actual IOR executable for traced hard-mode cold verification')
p.add_argument('--mpi-run',type=Path,help='MPI launcher paired with --ior-binary')
p.add_argument('--ior-clients',type=int,choices=(1,9),default=1,help='one or nine independent FUSE clients on this host')
p.add_argument('--ior-ranks',type=int,action='append',default=[])
p.add_argument('--ior-segments',type=int,default=3000)
p.add_argument('--fold-audit', action='store_true', help='require authoritative fold transitions and cold bytes after GC')
p.add_argument('--sequential', action='store_true', help='sustained same-mount writes beyond metadata-cache capacity')
p.add_argument('--memo', action='store_true', help='immediate lookup/stat following local mutations')
p.add_argument('--rename-cache', action='store_true', help='root rename evicts the exact old name')
p.add_argument('--mixed-reads', action='store_true', help='tiny demand reads while sequential prefetch uses a bounded cache')
p.add_argument('--ecrawl-binary',type=Path,help='run the actual ecrawl dense/sparse classification gate; requires --allocation')
p.add_argument('--allocation', action='store_true', help='cold non-writing client allocation reports for dense and sparse files')
p.add_argument('--compactor-pressure', action='store_true', help='fault-build follower pressure with finite stop and recovery')
p.add_argument('--du-audit',action='store_true',help='measure repeated GNU du on a cold never-writing client')
p.add_argument('--lookup-barrier',action='store_true',help='delay an actual LOOKUP row across local chmod')
p.add_argument('--storage-roots',type=int,choices=(1,2),default=1)
p.add_argument('--put-reply-fault',action='store_true',help='fault client loses accepted PUT replies and collides path hints')
p.add_argument('--opid-failover',action='store_true',help='kill the leader after a committed CREATE reply is lost')
p.add_argument('--report-audit', action='store_true', help='small-file publication trace and cold verification')
p.add_argument('--server-binary', type=Path, help='owned experimental/fault daemon; normal client/mgmt remain unchanged')
p.add_argument('--client-binary', type=Path, help='owned test-only FUSE binary')
p.add_argument('--worker-fault',choices=('allocation','tls','growth'))
p.add_argument('--gc-bulk-gib',type=int,choices=range(1,11),help='large live deletion with physical and pass accounting')
p.add_argument('--idle-seconds',type=int,default=0,help='measure live-table idle GC, process CPU and Raft commits')
p.add_argument('--full-node',action='store_true',help='one node quota exhausted while three fragment targets remain writable')
p.add_argument('--port', type=int, default=20190)
a = p.parse_args()
assert bool(a.ior_binary)==bool(a.mpi_run),'IOR and MPI launcher must be supplied together'
if a.ior_clients==9:assert a.ior_binary,'nine-client gate requires IOR'
if a.ior_binary:
    a.ior_binary=a.ior_binary.resolve();assert a.mpi_run and a.ior_segments>0
    a.mpi_run=a.mpi_run.resolve();assert a.ior_ranks and all(0<n<=36 for n in a.ior_ranks)
if a.ecrawl_binary:
    a.ecrawl_binary=a.ecrawl_binary.resolve();assert a.allocation
if a.server_binary:a.server_binary=a.server_binary.resolve()
if a.client_binary:a.client_binary=a.client_binary.resolve()
source = Path(__file__).resolve().parents[2]
if a.drain_pressure:assert a.publication_pressure and a.client_binary
memory_group=None;memory_before=None
if a.memory_limit_bytes:
    relative=Path('/proc/self/cgroup').read_text().strip().split('0::',1)[1]
    memory_group=Path('/sys/fs/cgroup')/relative.lstrip('/')
    ceiling=(memory_group/'memory.max').read_text().strip()
    assert ceiling!='max' and int(ceiling)==a.memory_limit_bytes,(memory_group,ceiling)
    assert (memory_group/'memory.swap.max').read_text().strip()=='0'
    memory_before=dict(line.split() for line in (memory_group/'memory.events').read_text().splitlines())
parents = a.root or ['/data1/efs', '/data2/efs']
assert len(parents) == 2
for port in range(a.port, a.port+4):
    with socket.socket() as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(('127.0.0.1', port))
work = Path(tempfile.mkdtemp(prefix='gc-'+a.mode+'-', dir=parents[0]))
other = Path(tempfile.mkdtemp(prefix='gc-'+a.mode+'-', dir=parents[1]))
roots = [work/'n1',work/'n2',other/'n3',other/'n4']
data_roots=[root/('storage'+str(i)) for root in roots for i in range(2)] if a.storage_roots==2 else roots
mount = work/'mnt'
mount.mkdir()
digest=hashlib.sha256()
inputs=[source/'Makefile'] + [x for folder in ('src','include') for x in (source/folder).rglob('*') if x.suffix in ('.c','.h')]
for path in sorted(inputs):
    digest.update(str(path.relative_to(source)).encode()+b'\0'+path.read_bytes())
(work/"manifest.json").write_text(json.dumps({
    "mode":a.mode,"roots":[str(x) for x in roots],
    "real_roots":[str(x.resolve()) for x in roots],"port":a.port,
    "gates":{key:str(value) if isinstance(value,Path) else value for key,value in vars(a).items()},"source_tree_sha256":digest.hexdigest(),
    "test_script_sha256":hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    "binary_sha256":{name:hashlib.sha256((a.server_binary if name=='efsd' and a.server_binary else a.client_binary if name=='efs-fuse' and a.client_binary else source/name).read_bytes()).hexdigest()
                      for name in ('efsd','efs-fuse','efs-mgmt')},
},indent=2))
env = dict(os.environ, EFS_MD_RAFT_N='4', EFS_TRANSPORT='tcp')
if a.gc_bulk_gib:
    assert not a.idle_seconds;env['EFS_GC_DBG']='1'
if a.ior_binary:env.update(EFS_DCACHE_TRACE='1',EFS_REPORT_DBG='1')
if a.workers:env['EFS_REPLY_BUFFER_TRACE']='1'
if a.worker_fault:
    assert a.workers and a.client_binary,'worker faults require explicit fault client and --workers'
    env['EFS_FAULT_REPLY_BUFFER']=a.worker_fault
hard_budget=(256 if a.production_budgets else 32)<<20
drain_budget=(64 if a.production_budgets else 32)<<20
if a.pressure or a.mixed_reads or a.publication_pressure:env.update(EFS_DCACHE_BYTES=str((128 if a.production_budgets else 16)<<20),EFS_DCACHE_HARD_BYTES=str(hard_budget),EFS_DCACHE_DRAIN_BYTES=str(drain_budget),EFS_BUF_BUDGET_TRACE='1')
if a.publication_pressure:
    assert a.client_binary,'publication pressure requires explicit fault client'
    env['EFS_FAULT_REPORT_FILE']=str(work/'report-fault')
    if a.drain_pressure:env['EFS_FAULT_DRAIN_FILE']=str(work/'drain-fault')
if a.lookup_barrier:
    assert a.client_binary,'LOOKUP barrier requires fault client'
    env['EFS_FAULT_LOOKUP_BARRIER']=str(work)
if a.put_reply_fault:
    assert a.client_binary,'PUT reply fault requires explicit fault client'
if a.opid_failover:assert a.opid and not a.opid_restart
servers=[]
client=None
peer=None
peer_mount=work/"peer"
peer_mount.mkdir()
handles=[]
extra_clients=[]
blocked=None

def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, timeout=kwargs.pop("timeout",60), env=env, **kwargs)
def status_available():
    return status(3)

def status(count=4):
    results=[]
    for i in range(count):
        text=run([source/'efs-mgmt','gc-status',f'127.0.0.1:{a.port+i}'], capture_output=True,text=True).stdout
        results.append([{k:int(v) for k,v in re.findall(r'(\w+)=(\d+)', line)} for line in text.splitlines()])
    return results

def wait(predicate, label, seconds=120):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        if predicate():
            print(label+' PASS',flush=True);return
        time.sleep(.25)
    (work/'failed-status.json').write_text(json.dumps(status(),indent=2))
    raise AssertionError(label+' timed out')

def start():
    for i,root in enumerate(roots):
        root.mkdir(exist_ok=True)
        log=open(work/f'n{i+1}.log','ab');handles.append(log)
        cmd=[a.server_binary or source/'efsd','--node-id',str(i+1),'--addr','127.0.0.1','--port',str(a.port+i),'--storage',root,'--quota','16G' if (a.ior_binary or a.gc_bulk_gib) else ('4G' if a.compactor_pressure else '1G'),'--'+('direct-io' if a.mode=='direct' else 'no-direct-io')]
        if a.storage_roots==2:
            first=root/'storage0';second=root/'storage1'
            first.mkdir(exist_ok=True);second.mkdir(exist_ok=True)
            cmd[cmd.index(root)]=first
            cmd += ['--storage',second]
        if i:cmd += ['--join',f'127.0.0.1:{a.port}']
        node_env=dict(env)
        if a.compactor_pressure:
            node_env['EFS_RAFT_OBS']='1'
            if i==3:node_env.update(EFS_FAULT_COMPACT_STALL='1',EFS_FAULT_COMPACT_FILE=str(work/'compactor-fault'))
        servers.append(subprocess.Popen([str(x) for x in cmd],env=node_env,stdout=log,stderr=log))
        def listening():
            if servers[-1].poll() is not None:raise AssertionError('daemon exited')
            try:
                with socket.create_connection(('127.0.0.1',a.port+i), timeout=.2):return True
            except OSError:return False
        wait(listening,f'node {i+1} listening',30)
    time.sleep(3)

def stop(crash=False):
    for proc in servers:
        if proc.poll() is None:proc.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
    for proc in servers:
        try:proc.wait(timeout=20)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
    servers.clear()

def mount_client():
    global client
    log=open(work/'fuse.log','ab');handles.append(log)
    client=subprocess.Popen([str(a.client_binary or source/'efs-fuse'),f'127.0.0.1:{a.port}',a.mount_label,str(mount),'-f',*(['-o','max_threads=16,max_idle_threads=1'] if a.workers else [])],env=env,stdout=log,stderr=log)
    def ready():
        if client.poll() is not None:raise AssertionError('FUSE exited')
        if subprocess.run(['mountpoint','-q',str(mount)]).returncode:return False
        return subprocess.run(['timeout','2','stat',str(mount)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0
    wait(ready,'client serving',40)

def unmount():
    global client
    if subprocess.run(['mountpoint','-q',str(mount)]).returncode==0:
        run([source/'scripts/client.sh','stop',mount])
    if client:
        client.wait(timeout=15);client=None

def files(ino):
    seg=[]
    for _ in range(5):seg.append(f'{ino%10000:04d}');ino//=10000
    found=[]
    for root in data_roots:
        folder=root/'data/exports/1'
        for part in reversed(seg):folder/=part
        found += [x for x in folder.glob('*/*') if x.is_file() and '.sum' not in x.name]
    return found

class Put(ctypes.Structure):
    _fields_=[('eid',ctypes.c_uint32),('ino',ctypes.c_uint64),('ci',ctypes.c_uint32),('fi',ctypes.c_uint32),('sum',ctypes.c_uint8*32),('length',ctypes.c_uint32),('gen',ctypes.c_uint64),('hint',ctypes.c_uint32)]

def recv_exact(sock,n):
    body=b''
    while len(body)<n:
        part=sock.recv(n-len(body))
        assert part,'short reply'
        body+=part
    return body

def valid_put(node,ino):
    req=Put(eid=1,ino=ino,ci=0,fi=0,length=65536,gen=987654321)
    # BLAKE3 of exactly 65536 'x' bytes; same vector as fragment_integrity.py.
    req.sum[:]=bytes.fromhex('70ff942c316810ac5ffe7081fc049dba30713646b1d6d7272cd5948ad5579804')
    payload=bytes(req)+b'x'*65536
    with socket.create_connection(('127.0.0.1',a.port+node),timeout=5) as sock:
        sock.sendall(struct.pack('>IB',len(payload)+1,6)+payload)
        length=struct.unpack('>I',recv_exact(sock,4))[0]
        return recv_exact(sock,length)

def create(name, body=None):
    path=mount/name
    fd=os.open(path,os.O_RDWR|os.O_CREAT|os.O_EXCL,0o600)
    if body is None:body=bytes(range(256))*4096
    for _ in range(9):assert os.write(fd,body)==len(body)
    os.fsync(fd)
    ino=os.fstat(fd).st_ino
    wait(lambda:len(files(ino))>=3,'fragment files present')
    return path,fd,ino

def totals(samples):
    return {
        'passes': sum(node[0]['passes'] for node in samples),
        'removed_fragments': sum(node[1]['removed_fragments'] for node in samples),
        'reclaimed_payload_bytes': sum(node[1]['reclaimed_payload_bytes'] for node in samples),
        'data_usage_bytes': sum(node[1]['data_usage_bytes'] for node in samples),
    }

def observe_pass(label):
    before = totals(status())
    wait(lambda: totals(status())['passes'] > before['passes'], label)
    return before

def drained():
    samples=[group for node in status() for group in node[2:] if group.get('sampled')]
    return {g['group'] for g in samples}=={0,2} and all(g['pending_estimate']==0 and g['reap_seen']==0 and g['orphan_seen']==0 for g in samples)

def chunk_state(ino):
    # Query all reachable replicas until a current authoritative reply exists.
    # BUSY/NOTPRIMARY are not a missing chunk or a zero-filled image.
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        for node in range(4):
            reply=subprocess.run([str(source/'efs-mgmt'),'raft-getchunks',f'127.0.0.1:{a.port+node}',str(ino)],env=env,capture_output=True,text=True,timeout=10)
            if reply.returncode==0 and 'status=0 ' in reply.stdout:
                match=re.search(r'ci=0 .*base_gen=(\d+).*spans=(\d+) seq=(\d+)',reply.stdout)
                if match:
                    state=dict(zip(('base_gen','spans','seq'),map(int,match.groups())))
                    lengths=[int(x) for x in re.findall(r'    span off=\d+ len=(\d+)',reply.stdout)]
                    assert len(lengths)==state['spans'],reply.stdout
                    state['live_spans']=sum(x>0 for x in lengths)
                    state['tombstones']=sum(x==0 for x in lengths)
                    return state
        time.sleep(.1)
    raise AssertionError('no authoritative chunk state: '+str(ino))

try:
    print('Evidence directory: '+str(work),flush=True)
    start()
    run([source/'efs-mgmt','raft-mkfs',f'127.0.0.1:{a.port}'])
    mount_client()
    if a.full_node:
        endpoint=f'127.0.0.1:{a.port}'
        result=run([source/'efs-mgmt','shrink-quota',endpoint,str((1<<30)-(64<<10))],capture_output=True,text=True)
        assert 'Reduced running quota' in result.stdout,result.stdout
        target=mount/'full-node-reroute';block=bytes(range(256))*4096
        with target.open('xb',buffering=0) as out:
            for _ in range(32):assert out.write(block)==len(block)
            os.fsync(out.fileno());ino=os.fstat(out.fileno()).st_ino
        limited=status()[0][1]['data_usage_bytes']
        assert limited<=64<<10,limited
        unmount();mount_client()
        with target.open('rb',buffering=0) as inp:
            for _ in range(32):assert inp.read(len(block))==block
            assert inp.read(1)==b''
        target.unlink();wait(lambda:not files(ino),'quota-full node reroute cold bytes and physical GC',180)
        (work/'full-node-reroute.json').write_text(json.dumps({'logical_bytes':32<<20,'limited_node_quota':64<<10,'limited_node_usage':limited,'cold_verified':True},indent=2))
        # Running quota shrink is not persisted; restart restores the fixture's
        # original quotas before later valid-PUT positive controls.
        unmount();stop();start();mount_client()
        print('one quota-full node preserves 2+1 writes on three remaining nodes PASS',flush=True)
    if a.namespace_cycles:
        outcomes=[]
        for index in range(16):
            first=mount/f'cycle-a-{index}';second=mount/f'cycle-b-{index}'
            first.mkdir();second.mkdir()
            ino_a=first.stat().st_ino;ino_b=second.stat().st_ino
            barrier=threading.Barrier(2)
            def move(name,parent):
                barrier.wait(timeout=10)
                for node in range(4):
                    reply=run([source/'efs-mgmt','raft-rename',f'127.0.0.1:{a.port+node}','1',name,str(parent),name],capture_output=True,text=True).stdout
                    status_value=int(re.search(r'status=(\d+)',reply).group(1))
                    if status_value!=7:return status_value
                raise AssertionError('no authoritative rename reply')
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                jobs=[pool.submit(move,first.name,ino_b),pool.submit(move,second.name,ino_a)]
                replies=[job.result() for job in jobs]
            assert replies.count(0)<=1,('directory cycle committed',replies)
            assert all(value in (0,1,5,6,11) for value in replies),replies
            # A concurrent BUSY pair is safe but does not establish progress.
            if 0 not in replies:
                def progress():
                    reply=run([source/'efs-mgmt','raft-rename',f'127.0.0.1:{a.port}','1',first.name,str(ino_b),first.name],capture_output=True,text=True).stdout
                    value=int(re.search(r'status=(\d+)',reply).group(1))
                    if value in (5,7,11):return False
                    assert value==0,reply
                    return True
                wait(progress,'concurrent directory guard retry makes progress',30)
                winner=0
            else:winner=replies.index(0)
            outcomes.append(replies)
            unmount();mount_client()
            child,parent=(first,second) if winner==0 else (second,first)
            assert not child.exists()
            assert (parent/child.name).stat().st_ino==(ino_a if winner==0 else ino_b)
            (parent/child.name).rmdir();parent.rmdir()
        (work/'namespace-cycle-races.json').write_text(json.dumps({'opposite_inode_parent_races':16,'statuses':outcomes},indent=2))
        print('opposite directory moves preserve acyclic ancestry and make progress PASS',flush=True)
    if a.idle_seconds:
        assert 1<=a.idle_seconds<=3600
        directory=mount/'idle-live-table';directory.mkdir()
        for index in range(4096):
            with (directory/str(index)).open('xb',buffering=0) as out:
                assert out.write(index.to_bytes(8,'little'))==8
                os.fsync(out.fileno())
        wait(drained,'live-table idle precondition GC drained')
        time.sleep(3)
        def process_ticks(proc):
            fields=Path(f'/proc/{proc.pid}/stat').read_text().split(')',1)[1].split()
            return int(fields[11])+int(fields[12])
        def commits():
            return [run([source/'efs-mgmt','raft-status',f'127.0.0.1:{a.port+i}'],capture_output=True,text=True).stdout for i in range(4)]
        def committed_tail(replies):
            result={}
            for node,(root,reply) in enumerate(zip(roots,replies)):
                bounds={int(g):int(i) for g,i in re.findall(r'group (\d+) hosted=1 .*?commit=(\d+)',reply)}
                for key,value in raft_commands(root/'mdraft/log/raft.log',bounds).items():
                    if key in result:assert result[key]==value,('replica disagreement',key)
                    result[key]=value
            return result
        before_commits=commits();before_tail=committed_tail(before_commits);before_ticks=[process_ticks(proc) for proc in servers]
        before_logs=[(work/f'n{i+1}.log').stat().st_size for i in range(4)]
        started=time.monotonic();next_notice=started
        while time.monotonic()-started<a.idle_seconds:
            time.sleep(max(0,min(10,a.idle_seconds-(time.monotonic()-started))))
            if time.monotonic()>=next_notice:
                print(f'live-table idle elapsed={time.monotonic()-started:.0f}s',flush=True);next_notice=time.monotonic()+60
        elapsed=time.monotonic()-started
        cpu=[100*(process_ticks(proc)-ticks)/os.sysconf('SC_CLK_TCK')/elapsed for proc,ticks in zip(servers,before_ticks)]
        slow=[]
        for index,offset in enumerate(before_logs):
            with (work/f'n{index+1}.log').open('rb') as log:log.seek(offset);text=log.read().decode(errors='replace')
            slow.extend({'node':index+1,'line':line} for line in text.splitlines() if 'gc-pass ' in line)
        after_commits=commits();after_tail=committed_tail(after_commits)
        new_entries={key:value for key,value in after_tail.items() if key not in before_tail}
        command_counts={}
        for _,command in new_entries.values():
            if command:command_counts[str(command[0])]=command_counts.get(str(command[0]),0)+1
        gc_ack_cmd=int(re.search(r'#define EFS_MD_CMD_GC_ACK\s+(\d+)',(source/'include/efs/meta_cmd.h').read_text()).group(1))
        assert command_counts.get(str(gc_ack_cmd),0)==0,command_counts
        record={'idle_committed_commands':command_counts,'idle_gc_ack_entries':command_counts.get(str(gc_ack_cmd),0),'idle_new_entries':len(new_entries),'live_files':4096,'elapsed_seconds':elapsed,'cpu_percent':cpu,'slow_gc_passes':slow,'before_raft':before_commits,'after_raft':after_commits}
        (work/'idle-live-table.json').write_text(json.dumps(record,indent=2))
        assert not slow and max(cpu)<5,record
        print('live-table idle: no slow GC passes; each daemon CPU below 5 percent PASS',flush=True)
        for path in directory.iterdir():path.unlink()
        directory.rmdir()
        wait(drained,'idle live-table bulk deletion drained',600)
    if a.report_audit:
        folder=mount/'report-audit';folder.mkdir()
        for index in range(2048):
            payload=index.to_bytes(8,'little')+bytes(range(256))*12
            with (folder/str(index)).open('xb',buffering=0) as out:
                assert out.write(payload)==len(payload)
                # Exercise close/flush as well as explicit fsync publication.
                if index%2==0:os.fsync(out.fileno())
        unmount();mount_client()
        for index in range(2048):
            path=folder/str(index)
            assert path.read_bytes()==index.to_bytes(8,'little')+bytes(range(256))*12
            path.unlink()
        folder.rmdir()
        text=(work/'fuse.log').read_text()
        misses=[line for line in text.splitlines() if 'putid miss' in line]
        (work/'report-identity.json').write_text(json.dumps({'files':2048,'cold_bytes_verified':True,'staging_identity_fallback_lines':misses},indent=2))
        assert not misses,misses[:10]
        print('small-file cold verification with zero staging-identity fallback PASS',flush=True)
        wait(drained,'bulk REPORT-audit deletions drained before fault fixture',600)
    if a.compactor_pressure:
        assert a.server_binary,'pressure gate requires an explicitly selected fault build'
        endpoint=f'127.0.0.1:{a.port+3}'
        def follower_state():
            text=run([source/'efs-mgmt','raft-status',endpoint],capture_output=True,text=True).stdout
            roles=re.findall(r'role=([-\w]+)',text)
            pairs=re.findall(r'commit=(\d+) applied=(\d+)',text)
            assert len(roles)==2 and len(pairs)==2 and 'FOLLOWER' in roles,text
            return roles,max(int(commit)-int(applied) for commit,applied in pairs)
        roles,lag=follower_state();assert 'LEADER' not in roles and lag==0,(roles,lag)
        def rss():
            return int(re.search(r'VmRSS:\s+(\d+)',Path(f'/proc/{servers[3].pid}/status').read_text()).group(1))
        cold_rss=rss();warm_paths=[]
        block=bytes(range(256))*4096
        for index in range(4):
            path=mount/f'compactor-warm-{index}'
            with path.open('xb',buffering=0) as out:
                for _ in range(128):assert out.write(block)==len(block)
                os.fsync(out.fileno())
            warm_paths.append(path)
        wait(lambda:follower_state()[1]==0,'warm follower caught up before pressure',120)
        time.sleep(6)
        baseline=rss();fault=work/'compactor-fault';fault.touch()
        wait(lambda:'kv-fault: compactor parked' in (work/'n4.log').read_text(),'owned follower compactor parked',30)
        samples=[];written=0;paths=[];bound='bytes-cap';started=time.monotonic()
        block=bytes(range(256))*4096
        try:
            for index in range(64):
                path=mount/f'compactor-pressure-{index}'
                with path.open('xb',buffering=0) as out:
                    for _ in range(128):assert out.write(block)==len(block)
                    os.fsync(out.fileno())
                paths.append(path);written+=128<<20
                text=(work/'n4.log').read_text()
                observations=re.findall(r'kv-obs: mt_bytes=(\d+) l0_bytes=(\d+) l0=(\d+) l1=(\d+)',text)
                roles,lag=follower_state()
                sample={'written_bytes':written,'rss_kib':rss(),'lag':lag,'roles':roles,
                        'kv':list(map(int,observations[-1])) if observations else None,
                        'all_node_rss_kib':[int(re.search(r'VmRSS:\s+(\d+)',Path(f'/proc/{proc.pid}/status').read_text()).group(1)) for proc in servers]}
                samples.append(sample)
                if 'LEADER' in roles:bound='follower-became-leader';break
                if 'kv-compact: backpressure' in text:bound='backpressure';break
                if sample['rss_kib']>2*baseline:bound='rss-2x';break
                if time.monotonic()-started>300:bound='time-cap';break
        finally:fault.unlink(missing_ok=True)
        recovery=time.monotonic()
        wait(lambda:follower_state()[1]==0,'parked follower catches up after release',120)
        result={'baseline_rss_kib':baseline,'cold_rss_kib':cold_rss,'preconditioned_bytes':512<<20,'bound':bound,'written_bytes':written,'samples':samples,
                'recovery_seconds':time.monotonic()-recovery,
                'pressure_observed':any(s['kv'] and s['kv'][2]>0 for s in samples)}
        (work/'compactor-pressure.json').write_text(json.dumps(result,indent=2))
        for path in paths+warm_paths:path.unlink()
        wait(drained,'bulk compactor-pressure deletions drained before fault fixture',600)
        print('finite compactor-pressure outcome '+json.dumps(result),flush=True)
    if a.memo:
        mutations=0
        for index in range(32):
            path=mount/f'memo-{index}'
            fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
            try:
                assert path.stat().st_size==0
                assert os.write(fd,b'memo-data')==9;os.fsync(fd)
                assert path.stat().st_size==9;mutations+=1
                os.chmod(path,0o640);assert path.stat().st_mode&0o777==0o640;mutations+=1
                target=mount/f'memo-link-{index}';os.link(path,target)
                assert path.stat().st_nlink==2;mutations+=1
                target.unlink();assert path.stat().st_nlink==1;mutations+=1
                timestamp=1700000000000000000+index
                os.utime(path,ns=(timestamp,timestamp))
                assert path.stat().st_mtime_ns==timestamp;mutations+=1
                os.ftruncate(fd,3);assert path.stat().st_size==3;mutations+=1
            finally:os.close(fd)
            path.unlink()
        (work/'memo-mutations.json').write_text(json.dumps({'files':32,'immediate_stat_checks':mutations},indent=2))
        print('immediate post-mutation lookup/stat gate PASS',flush=True)
    if a.rename_cache:
        for index in range(64):
            old=mount/f'root-old-{index}';new=mount/f'root-new-{index}'
            old.mkdir();(old/'child').write_bytes(b'child')
            inode=old.stat().st_ino
            old.rename(new)
            try:old.stat()
            except FileNotFoundError:pass
            else:raise AssertionError('renamed root name remains visible')
            assert new.stat().st_ino==inode and (new/'child').read_bytes()==b'child'
            # Reuse the old name to distinguish stale cache aliasing.
            old.mkdir();assert old.stat().st_ino!=inode
            old.rmdir();(new/'child').unlink();new.rmdir()
        print('root rename old-name miss, new inode and old-name reuse PASS',flush=True)
    if a.mixed_reads:
        run(['python3',source/'tests/live/read_pressure.py','prepare',mount,'--fixture','mixed-read-gate'],timeout=300)
        unmount();mount_client()
        result=run(['python3',source/'tests/live/read_pressure.py','run',mount,'--fixture','mixed-read-gate'],capture_output=True,text=True,timeout=180)
        (work/'mixed-read.json').write_text(result.stdout)
        print(result.stdout,flush=True)
        for path in (mount/'mixed-read-gate').iterdir():path.unlink()
        (mount/'mixed-read-gate').rmdir()
        wait(drained,'bulk mixed-read deletions drained before fault fixture',600)
        budgets=re.findall(r'buf-budget hard=(\d+) drain=(\d+) slab=(\d+)',(work/'fuse.log').read_text())
        assert budgets and all(tuple(map(int,b))==(hard_budget,drain_budget,32<<20) for b in budgets),budgets
    if a.gc_bulk_gib:
        def metadata_latency(label):
            result=run(['python3',source/'tests/measure/md_latency.py',mount],capture_output=True,text=True,timeout=120)
            (work/('gc-bulk-mdlat-'+label+'.log')).write_text(result.stdout+result.stderr)
            return {name:float(median) for name,median in re.findall(r'^(\S+)\s+min\s+\S+\s+med\s+(\S+)',result.stdout,re.M)}
        before_latency=metadata_latency('before')
        path=mount/'gc-bulk';body=bytes(range(256))*4096
        started=time.monotonic()
        with path.open('xb',buffering=0) as out:
            for _ in range(a.gc_bulk_gib*1024):assert out.write(body)==len(body)
            os.fsync(out.fileno());ino=os.fstat(out.fileno()).st_ino
        write_seconds=time.monotonic()-started
        assert path.stat().st_size==a.gc_bulk_gib*(1<<30)
        before_stats=status();before_totals=totals(before_stats)
        offsets=[(work/f'n{i+1}.log').stat().st_size for i in range(4)]
        started=time.monotonic();path.unlink()
        wait(drained,'bulk GC records drained',1800)
        assert not files(ino),'GC queues drained but physical bulk fragments remain'
        elapsed=time.monotonic()-started;after_stats=status();after_totals=totals(after_stats)
        scan_passes=[]
        for node,offset in enumerate(offsets):
            with (work/f'n{node+1}.log').open('rb') as log:log.seek(offset);text=log.read().decode(errors='replace')
            scan_passes.extend({'node':node+1,**{k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)}} for line in text.splitlines() if 'gc-pass ' in line)
        expected_fragments=a.gc_bulk_gib*(1<<30)//131072*3
        assert after_totals['removed_fragments']-before_totals['removed_fragments']>=expected_fragments
        assert after_totals['reclaimed_payload_bytes']-before_totals['reclaimed_payload_bytes']>=a.gc_bulk_gib*(1<<30)*3//2
        after_latency=metadata_latency('after')
        (work/'gc-bulk.json').write_text(json.dumps({'logical_bytes':a.gc_bulk_gib*(1<<30),'write_seconds':write_seconds,'deletion_seconds':elapsed,'before_stats':before_stats,'after_stats':after_stats,'before_totals':before_totals,'after_totals':after_totals,'scan_passes':scan_passes,'before_metadata_median_ms':before_latency,'after_metadata_median_ms':after_latency},indent=2))
        print(f'{a.gc_bulk_gib} GiB deletion physical inventory/queues and pass accounting PASS',flush=True)
    if a.ior_binary:
        run(['cc','-O2','-pthread',source/'tests/tools/hardscan.c','-o',work/'hardscan'])
        def start_ior_peers():
            for index in range(1,a.ior_clients):
                target=work/f'ior-client-{index}';target.mkdir(exist_ok=True)
                log=open(work/f'ior-client-{index}.log','ab');handles.append(log)
                proc=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{a.port}','default',str(target),'-f'],env=env,stdout=log,stderr=log)
                extra_clients.append((proc,target))
                def ready():
                    assert proc.poll() is None,'IOR peer exited'
                    return subprocess.run(['mountpoint','-q',str(target)]).returncode==0 and subprocess.run(['timeout','2','stat',str(target)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0
                wait(ready,f'IOR independent client {index+1} mounted',40)
        def stop_ior_peers():
            for proc,target in extra_clients:
                run([source/'scripts/client.sh','stop',target]);proc.wait(timeout=15)
            extra_clients.clear()
        wrapper=work/'ior-client-wrapper'
        wrapper.write_text('#!/usr/bin/env python3\nimport os,sys\nfrom pathlib import Path\nroots='+repr([str(mount)]+[str(work/f'ior-client-{i}') for i in range(1,a.ior_clients)])+'\nargs=sys.argv[1:]\ni=args.index("-o")+1\nrelative=Path(args[i]).relative_to(roots[0])\nargs[i]=str(Path(roots[int(os.environ["PMI_RANK"])//4])/relative)\nos.execv('+repr(str(a.ior_binary))+',['+repr(str(a.ior_binary))+',*args])\n')
        wrapper.chmod(0o700)
        records=[]
        for ranks in a.ior_ranks:
            if a.ior_clients==9:assert ranks==36
            start_ior_peers()
            path=mount/f'ior-hard-{ranks}';signature=271828
            base=[a.mpi_run,'-n',ranks,wrapper if a.ior_clients==9 else a.ior_binary,'-a','POSIX','--dataPacketType=timestamp','-C','-Q','1','-g',f'-G={signature}','-k','-e','-t','47008','-b','47008','-s',a.ior_segments,'-o',path]
            def ior_phase(phase,flags):
                started=time.monotonic()
                result=run([*base,*flags],capture_output=True,text=True,timeout=1800)
                (work/f'ior-{ranks}-{phase}.log').write_text(result.stdout+result.stderr)
                return time.monotonic()-started
            write_seconds=ior_phase('write',['-w'])
            assert path.stat().st_size==47008*ranks*a.ior_segments
            observe_pass('GC after IOR hard writes')
            # Purge all client caches, then independently check each packet.
            stop_ior_peers();unmount();mount_client();start_ior_peers()
            verify_seconds=ior_phase('cold-verify',['-r','-R'])
            stop_ior_peers();unmount();mount_client()
            result=run([work/'hardscan',path,ranks,a.ior_segments,signature,'8'],capture_output=True,text=True,timeout=1800)
            (work/f'ior-{ranks}-hardscan.log').write_text(result.stdout+result.stderr)
            assert f'DONE records={ranks*a.ior_segments} bad=0 short=0' in result.stdout,result.stdout
            ino=path.stat().st_ino;path.unlink()
            # Large scattered writes retain more immutable generations than
            # their logical size. Observe bounded queue drain, then inspect
            # the physical inventory once instead of repeatedly scanning it.
            wait(drained,'IOR GC records drained',1800)
            assert not files(ino),'IOR GC drained but physical fragments remain'
            records.append({'ranks':ranks,'segments':a.ior_segments,'records':ranks*a.ior_segments,'bytes':47008*ranks*a.ior_segments,'write_seconds':write_seconds,'verify_seconds':verify_seconds,'cold_hardscan_bad':0,'cold_hardscan_short':0})
            print(f'actual IOR hard ranks={ranks} cold verify/hardscan and deletion PASS',flush=True)
        records_meta={'independent_fuse_clients':a.ior_clients,'physical_client_hosts':1,'ior_binary':str(a.ior_binary),'ior_sha256':hashlib.sha256(a.ior_binary.read_bytes()).hexdigest(),'mpi_run_sha256':hashlib.sha256(a.mpi_run.read_bytes()).hexdigest(),'cases':records}
        (work/'ior-hard-acceptance.json').write_text(json.dumps(records_meta,indent=2))
    if a.allocation:
        dense=mount/'allocation-dense';dense.mkdir()
        sparse=mount/'allocation-sparse';sparse.mkdir()
        models=[]
        for name,offset,size in (('dense',0,1<<20),('dense-tail',0,131073),('dense-small',0,4096),('sparse',1<<30,4096),('sparse-16g',16<<30,4096),('empty',0,0)):
            path=(sparse if offset else dense)/name
            with path.open('xb',buffering=0) as out:
                if size:out.seek(offset);assert out.write(b'A'*size)==size
                os.fsync(out.fileno())
            models.append((path,offset,size))
        shrunk=dense/'dense-shrunk'
        with shrunk.open('xb',buffering=0) as out:
            assert out.write(b'A'*131072)==131072
            os.fsync(out.fileno());os.ftruncate(out.fileno(),1000);os.fsync(out.fileno())
        models.append((shrunk,0,1000))
        unmount();mount_client()  # new process has never written these inodes
        records=[]
        for path,offset,size in models:
            started=time.perf_counter_ns();st=path.stat();latency=(time.perf_counter_ns()-started)/1e6
            assert st.st_size==offset+size
            if size:
                assert st.st_blocks>0 and st.st_blocks*512<=((size+131071)//131072)*131072
                if not offset:assert st.st_blocks*512>=size,(path,st.st_size,st.st_blocks)
                with path.open('rb',buffering=0) as inp:
                    inp.seek(offset);assert inp.read(size)==b'A'*size
            else:assert st.st_blocks==0
            records.append({'name':path.name,'size':st.st_size,'blocks':st.st_blocks,'cold_stat_ms':latency})
        if a.ecrawl_binary:
            crawls=[]
            for folder,expected_sparse in ((dense,0),(sparse,2)):
                capture=work/('ecrawl-'+folder.name)
                result=run([a.ecrawl_binary,folder,capture],capture_output=True,text=True,timeout=120)
                (work/(capture.name+'.log')).write_text(result.stdout+result.stderr)
                summaries=[capture/'crawl_manifest.txt']
                fields=dict(re.findall(r'^(\w+)=(.*)$',result.stdout+'\n'+result.stderr,re.M))
                for summary in summaries:
                    if summary.is_file():fields.update(re.findall(r'^(\w+)=(.*)$',summary.read_text(),re.M))
                assert int(fields['files_sparse_heuristic'])==expected_sparse,(capture,fields)
                assert int(fields.get('total_errors',fields.get('errors','-1')))==0,(capture,fields)
                crawls.append({'tree':folder.name,'expected_sparse':expected_sparse,'fields':fields})
            (work/'ecrawl-allocation.json').write_text(json.dumps({'binary':str(a.ecrawl_binary),'sha256':hashlib.sha256(a.ecrawl_binary.read_bytes()).hexdigest(),'crawls':crawls},indent=2))
            print('actual ecrawl: dense zero false sparse, true sparse counted, no errors PASS',flush=True)
        for path,offset,size in models:path.unlink()
        dense.rmdir();sparse.rmdir()
        (work/'cold-allocation.json').write_text(json.dumps(records,indent=2))
        print('cold non-writing client dense/sparse/empty/truncated allocation PASS',flush=True)
    if a.sequential:
        # 16,384 chunk entries exceed the original 8 MiB metadata failure
        # threshold. A second file on the same mount tests continued admission.
        evidence=[]
        block=bytes(range(256))*4096
        for number,mib in enumerate((2048,512)):
            path=mount/f'sequential-admission-{number}'
            digest=hashlib.sha256()
            with path.open('xb',buffering=0) as out:
                for index in range(mib):
                    assert out.write(block)==len(block)
                    digest.update(block)
                os.fsync(out.fileno());ino=os.fstat(out.fileno()).st_ino
            evidence.append((path,ino,digest.hexdigest(),mib<<20))
        unmount();mount_client()
        for path,ino,expected_hash,size in evidence:
            digest=hashlib.sha256()
            with path.open('rb',buffering=0) as inp:
                while chunk:=inp.read(1<<20):digest.update(chunk)
            assert path.stat().st_size==size and digest.hexdigest()==expected_hash
            path.unlink()
            wait(lambda:not files(ino),'sequential admission cold bytes and physical deletion',180)
        (work/'sequential-admission.json').write_text(json.dumps({'files':2,'bytes':sum(x[3] for x in evidence),'same_mount':True,'cold_hashes_verified':True},indent=2))
    if a.mount_label!='default':
        text=(work/'fuse.log').read_text()
        assert 'is ignored' in text and "mounted export 'default'" in text,text
        print('legacy mount label explicitly ignored; canonical single export selected PASS',flush=True)
    if a.namespace or a.namespace_boundary:
        nested = mount/'deep-namespace';nested.mkdir()
        chain = [nested]
        for depth in range(20):
            nested /= f'd{depth}';nested.mkdir();chain.append(nested)
        moving=mount/'moving-dir';moving.mkdir()
        os.rename(moving,nested/'moved')
        assert (nested/'moved').is_dir()
        try:os.rename(chain[0],nested/'cycle')
        except OSError as error:assert error.errno==22,error
        else:raise AssertionError('ancestry cycle accepted')
        os.rmdir(nested/'moved')
        for folder in reversed(chain):os.rmdir(folder)
        print('twenty-ancestor rename preserves all guards and rejects cycle PASS',flush=True)
        helper=work/'names.c'
        helper.write_text(r'''#include "efs/kv_key.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char**argv){unsigned seen[64]={0},n=0,limit=argc>1?atoi(argv[1]):16;char name[32];for(unsigned i=0;n<limit;i++){snprintf(name,sizeof(name),"lane-%u",i);unsigned lane=efs_kv_dir_lane(name);if(!seen[lane]){seen[lane]=1;n++;printf("%u %s\n",lane,name);}}}
''')
        run(['cc','-I'+str(source/'include'),helper,source/'libefs.a','-pthread','-o',work/'names'])
        witnesses=run([work/'names'],capture_output=True,text=True).stdout.splitlines()
        assert len(witnesses)==16 and len({line.split()[0] for line in witnesses})==16
        def spread(folder):
            folder.mkdir();ino=folder.stat().st_ino
            for operation in ('begin','finish'):
                for node in range(4):
                    result=subprocess.run([str(source/'efs-mgmt'),'raft-dir',f'127.0.0.1:{a.port+node}',str(ino),operation],capture_output=True,text=True,timeout=30,env=env)
                    if result.returncode==0 and 'status=0' in result.stdout:break
                else:raise AssertionError('cannot force directory spread: '+result.stdout+result.stderr)
            # Administrative layout transition bypasses this mount's cache.
            unmount();mount_client()
            for line in witnesses:
                child=folder/line.split()[1];child.touch()
                try:child.unlink()
                except OSError:
                    print(f'failed unlink parent={ino} child={child.name}',flush=True)
                    for node in range(4):
                        for op,arguments in [('raft-lookup',[str(ino),child.name]),('raft-readdir',[str(ino)])]:
                            result=subprocess.run([str(source/'efs-mgmt'),op,f'127.0.0.1:{a.port+node}',*arguments],capture_output=True,text=True,timeout=30,env=env)
                            print(result.stdout+result.stderr,flush=True)
                    raise
            return ino
        empty=mount/'spread-empty';spread(empty);os.rmdir(empty)
        replacement=mount/'spread-replace';oldino=spread(replacement)
        source_dir=mount/'replace-source';source_dir.mkdir();srcino=source_dir.stat().st_ino
        os.rename(source_dir,replacement)
        actualino=replacement.stat().st_ino
        print(f'replacement inode: source={srcino} old={oldino} observed={actualino}',flush=True)
        for node in range(4):
            def replacement_matches():
                result=run([source/'efs-mgmt','raft-lookup',f'127.0.0.1:{a.port+node}','1','spread-replace'],capture_output=True,text=True).stdout
                if re.search(r'status=(5|7)\b',result):return False
                assert 'status=0' in result and f'ino={srcino} ' in result,result
                return True
            wait(replacement_matches,f'node {node+1} authoritative replacement inode',30)
        assert actualino==srcino
        os.rmdir(replacement)
        (work/'namespace-lanes.txt').write_text('\n'.join(witnesses)+'\n')
        print('sixteen used-lane empty HASHED rmdir and replacement PASS',flush=True)
        if a.namespace_boundary:
            witnesses=run([work/'names','64'],capture_output=True,text=True).stdout.splitlines()
            assert len(witnesses)==64
            boundary=mount/'full-lane-boundary';boundary_ino=spread(boundary)
            wait(drained,'namespace boundary prior GC drained')
            os.rmdir(boundary)
            replacement=mount/'full-lane-replacement';spread(replacement)
            incoming=mount/'full-lane-incoming';incoming.mkdir();saved=incoming.stat().st_ino
            os.rename(incoming,replacement)
            assert replacement.stat().st_ino==saved
            unmount();stop();start();mount_client()
            assert not boundary.exists() and replacement.stat().st_ino==saved
            os.rmdir(replacement)
            (work/'namespace-full-lanes.json').write_text(json.dumps({'lanes':64,'rmdir':True,'replacement':True,'restart_verified':True},indent=2))
            print('64-lane empty directory removal/replacement and restart PASS',flush=True)

    if a.namespace_max:
        # Find the actual participant envelope before the separate 64-hop cap.
        folder=mount;chain=[];moving=mount/'max-moving';moving.mkdir()
        last=moving;elapsed=None
        for index in range(64):
            folder=folder/f'max-{index}';folder.mkdir();chain.append(folder)
            started=time.monotonic()
            try:os.rename(last,folder/'moved')
            except OSError as error:
                assert error.errno in (11,16),error
                elapsed=time.monotonic()-started;break
            last=folder/'moved'
        assert elapsed is not None and len(chain)>20
        saved=last.stat().st_ino
        assert elapsed<30 and not (folder/'moved').exists()
        unmount();stop();start();mount_client()
        assert last.stat().st_ino==saved
        recovered=mount/'max-recovered';os.rename(last,recovered)
        assert recovered.stat().st_ino==saved;os.rmdir(recovered)
        for directory in reversed(chain):os.rmdir(directory)
        (work/'namespace-max.json').write_text(json.dumps({'accepted_depth':len(chain)-1,'refused_depth':len(chain),'refusal_seconds':elapsed,'restart_verified':True},indent=2))
        print(f'namespace actual envelope: depth={len(chain)-1} accepted, depth={len(chain)} refused in {elapsed:.3f}s, restart recovery PASS',flush=True)
    if a.integrity:
        # A swap preserves a valid local payload/digest pair but violates the
        # checksum recorded for this immutable object in committed metadata.
        target, fd, target_ino = create('integrity-target');os.close(fd)
        donor, fd, donor_ino = create('integrity-donor', b'z'*(1024*1024));os.close(fd)
        originals = {}
        donors = {int(path.name.split('.')[1]):path.read_bytes()
                  for path in files(donor_ino) if path.name.startswith('0.')}
        targets = {int(path.name.split('.')[1]):path
                   for path in files(target_ino) if path.name.startswith('0.')}
        assert set(targets)=={0,1,2} and set(donors)=={0,1,2}
        expected_bytes = bytes(range(256))*4096*9
        for fi in (0,1):originals[fi]=targets[fi].read_bytes()
        targets[0].write_bytes(donors[0])
        unmount();mount_client()
        assert target.read_bytes()==expected_bytes
        print('one swapped fragment falls back to trusted parity PASS',flush=True)
        targets[1].write_bytes(donors[1])
        unmount();mount_client()
        try:
            target.read_bytes()
        except OSError as error:
            assert error.errno == 5,error
        else:raise AssertionError('two swapped objects returned silent wrong bytes')
        for fi,body in originals.items():targets[fi].write_bytes(body)
        unmount();mount_client()
        assert target.read_bytes()==expected_bytes
        os.unlink(target);os.unlink(donor)
        wait(lambda:not files(target_ino) and not files(donor_ino),'integrity fixtures reclaimed')
        print('two swapped fragments fail read; restored bytes verify cold PASS',flush=True)
    if a.du_audit:
        directory=mount/'du-audit';directory.mkdir()
        for index in range(128):
            with (directory/str(index)).open('xb',buffering=0) as out:
                assert out.write(bytes([index+1])*4096)==4096
                os.fsync(out.fileno())
        unmount();mount_client()
        allocated=[path.stat().st_blocks*512 for path in directory.iterdir()]
        assert len(allocated)==128 and all(4096<=size<=128*1024 for size in allocated),allocated
        expected=sum(allocated)+directory.stat().st_blocks*512
        samples=[]
        for index in range(5):
            started=time.monotonic()
            result=run(['du','-s','--block-size=1',directory],capture_output=True,text=True)
            samples.append(time.monotonic()-started)
            assert int(result.stdout.split()[0])==expected,(result.stdout,expected)
        (work/'du-audit.json').write_text(json.dumps({'files':128,'allocated_bytes':expected,'elapsed_seconds':samples,'tool':'GNU du; not the unidentified Spark workload'},indent=2))
        print(f'cold-client GNU du: 128 files, allocated={expected}, seconds={samples} PASS',flush=True)
        for path in directory.iterdir():path.unlink()
        directory.rmdir();wait(drained,'du-audit physical deletion drained',600)
    if a.lookup_barrier:
        target,fd,target_ino=create('memo-blocked',b'memo-race')
        os.close(fd);unmount();mount_client()
        fd=os.open(target,os.O_RDWR)
        (work/'lookup-arm').touch()
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            stale=pool.submit(os.stat,target)
            deadline=time.monotonic()+10
            while not (work/'lookup-entered').exists():
                assert time.monotonic()<deadline,'LOOKUP barrier not reached'
                time.sleep(.01)
            try:os.fchmod(fd,0o640)
            finally:(work/'lookup-arm').unlink()
            old=stale.result(timeout=10)
        current=os.stat(target)
        assert current.st_mode&0o777==0o640,(old,current)
        os.close(fd);target.unlink()
        wait(lambda:not files(target_ino),'lookup barrier file reclaimed')
        (work/'lookup-barrier.json').write_text(json.dumps({'old_mode':old.st_mode,'new_mode':current.st_mode},indent=2))
        print('actual LOOKUP reply spanning chmod cannot repopulate stale memo PASS',flush=True)
    if a.put_reply_fault:
        # Enable this one-object fault on a fresh process, independently of
        # earlier memo tests/remounts. Preserve old process logs as evidence.
        unmount();env['EFS_FAULT_PUT_REPLY_COLLISION']='1';mount_client()
        log_offset=(work/'fuse.log').stat().st_size
        probe,fd,probe_ino=create('ambiguous-put-fuse',bytes(range(256))*4096)
        os.close(fd)
        with (work/'fuse.log').open('rb') as log:
            log.seek(log_offset);text=log.read().decode()
        faults=re.findall(r'put-reply fault ino=(\d+) ci=(\d+) fi=(\d+) collision=1',text)
        assert len(faults)==3,faults
        target,ci=faults[0][:2]
        attempts=re.findall(r'put-attempt ino='+target+r' ci='+ci+r' fi=(\d+) gen=(\d+) first=(\d+) hint=(\d+)',text)
        retried=[x for x in attempts if x[2]=='0']
        assert len(retried)>=3 and all(x[3]=='0' for x in retried),attempts
        assert len({x[1] for x in attempts})==1,attempts
        # Inspect both roots for duplicate immutable fragment identities.
        identities={}
        for root in roots:
            for path in root.rglob('*'):
                if path.is_file() and re.match(r'\d+\.\d+\.',path.name):
                    key=(path.parent.name,path.name)
                    assert key not in identities,(key,path,identities.get(key))
                    identities[key]=str(path)
        usage=totals(status())['data_usage_bytes']
        assert usage==len(identities)*65536,(usage,len(identities))
        unmount();env.pop('EFS_FAULT_PUT_REPLY_COLLISION');mount_client()
        assert probe.read_bytes()==bytes(range(256))*4096*9
        probe.unlink();wait(lambda:not files(probe_ino),'ambiguous PUT fixture reclaimed')
        (work/'put-reply-collision.json').write_text(json.dumps({'faults':faults,'attempts':attempts,'unique_objects':len(identities),'data_usage_bytes':usage},indent=2))
        print('actual FUSE accepted-reply loss/collision: retries probe same generation; cold bytes and reclaim PASS',flush=True)
    if a.workers:
        # Prior cold integrity reads can leave kernel readahead callbacks in
        # flight. Give the retirement/RSS gate its own fresh client process.
        unmount();mount_client()
        body=bytes(range(256))*4096
        sample,fd,sample_ino=create('worker-read',body)
        directory=mount/'worker-readdir';directory.mkdir()
        for index in range(64):(directory/f'n{index}').touch()
        rss=[];fault_errors=[]
        def worker_read(index):
            deadline=time.monotonic()+30
            while True:
                try:
                    assert os.pread(fd,32768,1)==(body*9)[1:32769]
                    assert len(os.listdir(directory))==64
                    return
                except OSError as error:
                    assert a.worker_fault and error.errno==12,error
                    fault_errors.append(error.errno)
                    assert time.monotonic()<deadline,'faulted worker failed to recover'
        with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:
            for cycle in range(40):
                list(pool.map(worker_read,range(64)))
                time.sleep(.03)
                text=(Path('/proc')/str(client.pid)/'status').read_text()
                rss.append(int(re.search(r'VmRSS:\s+(\d+)',text).group(1)))
        time.sleep(.3)
        events=re.findall(r'reply-buffer (new|grow|retire) owner=(\S+) read=(\d+) readdir=(\d+) pid=(\d+)',(work/'fuse.log').read_text())
        owners={};retired=0;new=0;retired_bytes=0
        for event,owner,read,directory_bytes,pid in events:
            if int(pid)!=client.pid:continue # earlier remounts have exited address spaces
            capacities=(int(read),int(directory_bytes))
            if event=='new':
                assert owner not in owners,(event,owner);owners[owner]=capacities;new+=1
            elif event=='grow':
                assert owner in owners,(event,owner);owners[owner]=capacities
            else:
                assert owners.pop(owner)==capacities,(event,owner)
                retired+=1;retired_bytes+=sum(capacities)
        assert new>=16 and retired>=16 and retired_bytes>0,(new,retired,retired_bytes)
        assert len(owners)<=2,owners
        assert max(rss[5:])-min(rss[5:])<32*1024,rss
        injected=len(re.findall(r'reply-buffer fault kind=\d+ pid='+str(client.pid)+r'\b',(work/'fuse.log').read_text()))
        if a.worker_fault:assert injected>0 and fault_errors,(injected,fault_errors)
        (work/'worker-retirement.json').write_text(json.dumps({'new':new,'retired':retired,'retired_capacity_bytes':retired_bytes,'live_owners':len(owners),'rss_kib':rss,'fault_kind':a.worker_fault,'injected_failures':injected,'observed_enomem':len(fault_errors)},indent=2))
        print(f'real worker retirement: new={new} retired={retired} live={len(owners)} retired_bytes={retired_bytes} RSS_span_kib={max(rss[5:])-min(rss[5:])} PASS',flush=True)
        os.close(fd)
        if a.worker_fault:
            unmount();env.pop('EFS_FAULT_REPLY_BUFFER');mount_client()
        sample.unlink()
        for child in directory.iterdir():child.unlink()
        directory.rmdir()
        wait(lambda:not files(sample_ino),'worker fixture fragments reclaimed')
    if a.publication_pressure:
        path=mount/'failed-publication-pressure'
        fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
        ino_value=os.fstat(fd).st_ino
        fault=work/'report-fault';fault.write_text('WITHHOLD ALL\n')
        model=[];errors=[];samples=[];started=time.monotonic()
        try:
            for slot in range(4096 if a.production_budgets else 512):
                data=bytes([slot%251+1])*4096
                offset=slot*(1<<20)
                try:
                    assert os.pwrite(fd,data,offset)==len(data)
                    model.append((offset,data))
                    if a.drain_pressure and len(model)==32:(work/'drain-fault').touch()
                except OSError as error:
                    assert error.errno in (5,11,12,16),error
                    errors.append(error.errno)
                    text=Path(f'/proc/{client.pid}/status').read_text()
                    samples.append(int(re.search(r'VmRSS:\s+(\d+)',text).group(1)))
                    break
                text=Path(f'/proc/{client.pid}/status').read_text()
                samples.append(int(re.search(r'VmRSS:\s+(\d+)',text).group(1)))
            assert model and errors,'failed publication did not cause finite admission refusal'
            assert time.monotonic()-started<180
            # Failed fsync must not claim success while records are withheld.
            try:os.fsync(fd)
            except OSError as error:errors.append(error.errno)
            else:raise AssertionError('withheld publication falsely acknowledged fsync')
            log=(work/'fuse.log').read_text()
            charges=re.findall(r'dcache-pressure live=(\d+) reserved=(\d+) backing=(\d+) limit=(\d+)',log)
            assert charges,'bounded admission was not observed'
            assert all(int(live)+int(reserved)<=int(limit) and int(backing)<=int(limit) for live,reserved,backing,limit in charges),charges
            assert max(samples)<512*1024,samples
            if a.drain_pressure:
                held=re.findall(r'buf-fault drain held=(\d+) live=(\d+) reserved=(\d+) backing=(\d+) limit=(\d+)',log)
                assert held,'no real recovery scratch exhaustion observed'
                assert any(int(live)+int(reserved)==int(limit) for count,live,reserved,backing,limit in held),held
                assert all(int(count)>0 and int(backing)<=int(limit) for count,live,reserved,backing,limit in held),held
        finally:
            (work/'drain-fault').unlink(missing_ok=True)
            fault.write_text('OFF\n')
        os.fsync(fd)
        assert os.fstat(fd).st_size==model[-1][0]+len(model[-1][1]),'refused write extended the file'
        os.close(fd)
        # Admission must recover on this SAME mount, before cold remount.
        recovered=mount/'publication-recovered';recovered.write_bytes(b'recovered')
        with recovered.open('rb') as inp:os.fsync(inp.fileno())
        # New admission and a cold read must succeed after the failure releases.
        unmount();mount_client()
        with path.open('rb',buffering=0) as inp:
            for offset,data in model:inp.seek(offset);assert inp.read(len(data))==data
        assert recovered.read_bytes()==b'recovered'
        record={'accepted_writes':len(model),'errors':errors,'peak_rss_kib':max(samples),'pressure_charges':charges,'cold_bytes_verified':True,'same_mount_admission_recovered':True,'drain_exhaustion':held if a.drain_pressure else None}
        (work/'publication-pressure.json').write_text(json.dumps(record,indent=2))
        path.unlink();recovered.unlink()
        wait(lambda:not files(ino_value),'failed-publication owned bodies cold verified and reclaimed',180)
        print('failed publication finite admission, retained bytes and recovery '+json.dumps(record)+' PASS',flush=True)
    if a.pressure:
        pressure=[];retries=[];samples=[]
        def sparse_writer(index):
            path=mount/f'sparse-pressure-{index}'
            fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
            retry_count=0;model=[]
            try:
                for slot in range(32):
                    offset=(1<<30)+slot*(1<<20)
                    data=bytes([(index*32+slot)%251+1])*4096
                    deadline=time.monotonic()+30
                    while True:
                        try:
                            assert os.pwrite(fd,data,offset)==len(data);break
                        except OSError as error:
                            assert error.errno in (11,12,16),error
                            retry_count+=1
                            assert time.monotonic()<deadline,'sparse admission made no progress'
                            os.fsync(fd);time.sleep(.01)
                    model.append((offset,data))
                os.fsync(fd)
                info=os.fstat(fd)
                assert info.st_size==model[-1][0]+4096,info
                return path,info.st_ino,model,retry_count
            finally:os.close(fd)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            jobs=[pool.submit(sparse_writer,index) for index in range(8)]
            while not all(job.done() for job in jobs):
                text=(Path('/proc')/str(client.pid)/'status').read_text()
                samples.append(int(re.search(r'VmRSS:\s+(\d+)',text).group(1)))
                time.sleep(.05)
            pressure=[job.result() for job in jobs]
        assert samples and max(samples)<512*1024,samples
        unmount();mount_client()
        for path,ino_value,model,retry_count in pressure:
            fd=os.open(path,os.O_RDONLY)
            try:
                for offset,data in model:assert os.pread(fd,len(data),offset)==data
                assert os.pread(fd,4096,0)==bytes(4096)
            finally:os.close(fd)
        before_delete=totals(status())
        for path,ino_value,model,retry_count in pressure:path.unlink()
        wait(lambda:all(not files(item[1]) for item in pressure),'sparse-pressure fragments physically reclaimed')
        after_delete=totals(status())
        assert after_delete['removed_fragments']>before_delete['removed_fragments']
        assert after_delete['reclaimed_payload_bytes']>before_delete['reclaimed_payload_bytes']
        budgets=re.findall(r'buf-budget hard=(\d+) drain=(\d+) slab=(\d+)',(work/'fuse.log').read_text())
        assert budgets and all(tuple(map(int,b))==(hard_budget,drain_budget,32<<20) for b in budgets),budgets
        record={'hard_body_bytes':hard_budget,'drain_bytes':drain_budget,'effective_budget_trace':budgets,'files':8,'writes':256,'retry_count':sum(item[3] for item in pressure),'peak_rss_kib':max(samples),'before_delete':before_delete,'after_delete':after_delete}
        (work/'sparse-pressure.json').write_text(json.dumps(record,indent=2))
        print(f'sparse concurrent admission, cold bytes/holes and physical reclaim: peak_RSS_kib={max(samples)} retries={record["retry_count"]} PASS',flush=True)
    if a.opid:
        helper=work/'opid-client.c'
        helper.write_text(r'''#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/opid.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
static unsigned port;
static efs_ino_t call(unsigned type,const void*req,unsigned size,unsigned seq){
    unsigned char packet[1024];struct efs_opid_req q={0};
    assert(size+EFS_OPID_WIRE_LEN<=sizeof(packet));memcpy(packet,req,size);
    q.id.client_uuid[0]=1;unsigned pid=getpid();memcpy(q.id.client_uuid+4,&pid,sizeof(pid));
    q.id.session_epoch=1;q.id.seq=seq;q.ack=seq-1;
    efs_opid_req_pack(&q,packet+size);
    for(unsigned phase=0;phase<2;phase++){
        int completed=0;
        for(unsigned attempt=0;attempt<64;attempt++){
            int fd=efs_connect_tcp("127.0.0.1",port);assert(fd>=0);
            efs_set_recv_timeout(fd,5000);efs_set_send_timeout(fd,5000);
            assert(efs_send_msg(fd,type,packet,size+EFS_OPID_WIRE_LEN)==0);
            void*body=NULL;unsigned len=0;unsigned char reply_type=0;
            int rc=efs_recv_msg(fd,&reply_type,&body,&len);close(fd);
            if(rc){free(body);assert(phase==0);completed=1;break;}
            assert(reply_type==type+1 && len==sizeof(struct efs_msg_inode_reply));
            struct efs_msg_inode_reply reply;memcpy(&reply,body,sizeof(reply));free(body);
            if(reply.status==EFS_INODE_RPC_BUSY || reply.status==EFS_INODE_RPC_NOT_PRIMARY){usleep(10000);continue;}
            assert(phase==1 && reply.status==EFS_INODE_RPC_OK);
            return reply.inode.ino;
        }
        assert(completed);
    }
    abort();
}
int main(int argc,char**argv){assert(argc==2);port=atoi(argv[1]);
    struct efs_msg_inode_create create={0};create.export_id=1;create.parent=1;create.mode=S_IFREG|0600;strcpy(create.name,"opid-source");
    efs_ino_t ino=call(EFS_MSG_INODE_CREATE,&create,sizeof(create),1);assert(ino);
    struct efs_msg_inode_link link={0};link.export_id=1;link.src_ino=ino;link.new_parent=1;strcpy(link.new_name,"opid-link");
    assert(call(EFS_MSG_INODE_LINK,&link,sizeof(link),2)==ino);
    struct efs_msg_inode_rename_at rename={0};rename.export_id=1;rename.old_parent=rename.new_parent=1;strcpy(rename.old_name,"opid-source");strcpy(rename.new_name,"opid-renamed");
    assert(call(EFS_MSG_INODE_RENAME_AT,&rename,sizeof(rename),3)==ino);
    struct efs_msg_inode_unlink unlink={0};unlink.export_id=1;unlink.parent=1;strcpy(unlink.name,"opid-renamed");
    assert(call(EFS_MSG_INODE_UNLINK,&unlink,sizeof(unlink),4)==ino);
    puts("CREATE/LINK/RENAME/UNLINK lost replies recovered with same operation identity PASS");
}
''')
        run(['cc','-I'+str(source/'include'),helper,source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',work/'opid-client'])
        listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen();listener.settimeout(.2)
        proxy_port=listener.getsockname()[1];proxy_stop=threading.Event();proxy_errors=[];dropped={};replayed={}
        target=a.port
        def proxy():
            endpoint=target
            while not proxy_stop.is_set():
                try:client_socket,_=listener.accept()
                except socket.timeout:continue
                except OSError:break
                try:
                    with client_socket, socket.create_connection(('127.0.0.1',endpoint),5) as upstream:
                        length=recv_exact(client_socket,4);request=recv_exact(client_socket,struct.unpack('>I',length)[0]);kind=request[0]
                        upstream.sendall(length+request)
                        reply_length=recv_exact(upstream,4);reply=recv_exact(upstream,struct.unpack('>I',reply_length)[0])
                        if reply[1]==7:
                            primary=struct.unpack_from('=I',reply,5)[0]
                            if primary: endpoint=a.port+primary-1
                        if reply[1]==0 and kind not in dropped:
                            dropped[kind]=(request,reply)
                            if a.opid_failover and kind==45:
                                for index,proc in enumerate(servers):
                                    reply_status=run([source/'efs-mgmt','raft-status',f'127.0.0.1:{a.port+index}'],capture_output=True,text=True).stdout
                                    if re.search(r'group 0 hosted=1 role=LEADER ',reply_status):
                                        proc.kill();proc.wait(timeout=10);dead=index;break
                                else:raise AssertionError('no leader to fail over')
                                endpoint=a.port+next(i for i in range(4) if i!=dead)
                                deadline=time.monotonic()+60
                                while True:
                                    reply_status=run([source/'efs-mgmt','raft-status',f'127.0.0.1:{endpoint}'],capture_output=True,text=True).stdout
                                    if 'group 0 hosted=1 role=LEADER ' in reply_status:break
                                    leader_match=re.search(r'group 0 .*leader=(\d+)',reply_status)
                                    if leader_match and int(leader_match[1])>=0:
                                        candidate=int(leader_match[1])
                                        if candidate!=dead:endpoint=a.port+candidate
                                    assert time.monotonic()<deadline,'failover election timed out'
                                    time.sleep(.1)
                                print('lost committed CREATE replay crosses leader death PASS',flush=True)
                            if a.opid_restart:
                                client_socket.close()
                                stop(crash=True);start();endpoint=a.port
                            continue # commit reply deliberately lost before client sees bytes
                        if reply[1]==0:
                            assert kind in dropped and request==dropped[kind][0]
                            assert reply[9:17]==dropped[kind][1][9:17] # native reply inode identity
                            replayed[kind]=True
                        client_socket.sendall(reply_length+reply)
                except Exception as error:proxy_errors.append(repr(error));proxy_stop.set()
        proxy_thread=threading.Thread(target=proxy);proxy_thread.start()
        try:
            run([work/'opid-client',proxy_port],timeout=120 if a.opid_restart else 60)
            assert not proxy_errors,proxy_errors
            if a.opid_failover:unmount();stop();start();mount_client()
            assert len(dropped)==len(replayed)==4,(dropped.keys(),replayed.keys())
            for name in ('opid-source','opid-renamed'):
                def absent():
                    reply=run([source/'efs-mgmt','raft-lookup',f'127.0.0.1:{target}','1',name],capture_output=True,text=True).stdout
                    if re.search(r'status=(5|7)\b',reply):return False
                    assert 'status=1' in reply,reply
                    return True
                wait(absent,'lost-reply namespace absent after replay',30)
            (work/'namespace-lost-replies.json').write_text(json.dumps({'dropped_reply_types':sorted(dropped),'replayed_types':sorted(replayed),'unchanged_request_identity':True,'crash_restart_between_commit_and_replay':a.opid_restart,'leader_death_after_create_commit':a.opid_failover},indent=2))
        finally:
            proxy_stop.set();listener.close();proxy_thread.join(5)
    if a.opid:
        linked=mount/'opid-link'
        assert linked.stat().st_nlink==1
        linked.unlink()
    # Two peers repeatedly touch adjacent ranges until the span chain folds.
    peer_log=open(work/'peer-fuse.log','ab');handles.append(peer_log)
    peer=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{a.port}','default',str(peer_mount),'-f'],env=env,stdout=peer_log,stderr=peer_log)
    wait(lambda:subprocess.run(['mountpoint','-q',str(peer_mount)]).returncode==0,'peer mounted',40)
    fold=mount/'peer-fold';f1=os.open(fold,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
    os.write(f1,b'\0'*(128*1024));os.fsync(f1)
    f2=os.open(peer_mount/'peer-fold',os.O_RDWR)
    model=bytearray(128*1024)
    fold_states=[]
    if a.fold_audit:fold_states.append(chunk_state(os.fstat(f1).st_ino))
    for i in range(20):
        fd=f1 if i%2==0 else f2
        off=0 if i%2==0 else 4096
        data=bytes([i+1])*4096
        assert os.pwrite(fd,data,off)==len(data);os.fsync(fd)
        model[off:off+len(data)]=data
        if a.fold_audit:fold_states.append(chunk_state(os.fstat(f1).st_ino))
    if a.fold_audit:
        assert any(x['live_spans']>0 for x in fold_states),fold_states
        transitions=[(old,new) for old,new in zip(fold_states,fold_states[1:]) if new['base_gen']!=old['base_gen'] and new['live_spans']<old['live_spans'] and new['tombstones']>old['tombstones']]
        assert transitions,('no observed fold',fold_states)
        (work/'fold-transitions.json').write_text(json.dumps({'ino':os.fstat(f1).st_ino,'states':fold_states,'folds':len(transitions)},indent=2))
    fino=os.fstat(f1).st_ino;os.close(f1);os.close(f2)
    run([source/'scripts/client.sh','stop',peer_mount]);peer.wait(timeout=15);peer=None
    print('two-peer adjacent writes and repeated folds complete',flush=True)
    # Retain a rewritten live file and verify it after GC and a cold restart.
    sentinel,fd,sino=create('sentinel')
    assert os.pwrite(fd,b'changed-span',135000)==12
    os.fsync(fd);os.close(fd)
    expected=hashlib.sha256(sentinel.read_bytes()).hexdigest()
    # Exercise tails assigned to both metadata groups; a span-only chunk
    # must survive shrink and subsequent GC without a fabricated zero base.
    for index in range(12):
        tail=mount/f'span-tail-{index}'
        fd=os.open(tail,os.O_CREAT|os.O_RDWR,0o600)
        assert os.write(fd,b'A'*10000)==10000;os.fsync(fd)
        tail_ino=os.fstat(fd).st_ino
        os.ftruncate(fd,4000);os.close(fd)
        assert tail.read_bytes()==b'A'*4000
        os.unlink(tail)
        wait(lambda:not files(tail_ino),'span-only truncate then physical reclaim')
    path,fd,ino=create('held')
    link=mount/'hardlink';os.link(path,link);os.unlink(path)
    observe_pass('GC runs while hardlink remains');assert files(ino) and os.pread(fd,16,0)==bytes(range(16))
    print('hardlink protects stored fragments PASS',flush=True)
    os.unlink(link);observe_pass('GC runs while unlinked inode is open')
    assert files(ino) and os.pread(fd,16,0)==bytes(range(16))
    assert os.pwrite(fd,b'held-write',100)==10;os.fsync(fd)
    print('open unlinked inode retains readable/writable fragments PASS',flush=True)
    before=0
    for x in files(ino):
        try:before+=x.stat().st_blocks*512
        except FileNotFoundError:pass  # overwritten generation may be collected

    checkpoint = totals(status())
    os.close(fd)
    wait(lambda:not files(ino),'last close physically reclaims fragments')
    def reclamation_observed():
        after = totals(status())
        return (after['passes'] > checkpoint['passes'] and
                after['removed_fragments'] > checkpoint['removed_fragments'] and
                after['reclaimed_payload_bytes'] > checkpoint['reclaimed_payload_bytes'] and
                after['data_usage_bytes'] < checkpoint['data_usage_bytes'])
    wait(reclamation_observed, 'GC passes, removal bytes and quota reflect last-close deletion')
    (work/'last-close-gc-effect.json').write_text(json.dumps({
        'ino': ino, 'before': checkpoint, 'after': totals(status()),
        'remaining_target_fragments': len(files(ino))}, indent=2))
    assert before>0
    print(f'closed-unlinked inode allocated bytes: {before} -> 0 PASS',flush=True)
    # A failed disk read must retain the durable GC record, not ACK it away.
    path,fd,ino=create('fault');os.close(fd)
    for node in range(4):
        reply=valid_put(node,ino)
        assert len(reply) in (2,3) and reply[:2]==bytes([7,0]),reply
    print('valid late-PUT control accepted on live inode by every member PASS',flush=True)
    blocked=files(ino)[0];blocked.chmod(0);blocked.parent.chmod(0o500)
    errors=sum(n[1]['delete_errors'] for n in status())
    os.unlink(path)
    wait(lambda:sum(n[1]['delete_errors'] for n in status())>errors,'disk failure visible')
    assert blocked.exists()
    time.sleep(2);assert blocked.exists()
    print('failed fragment remains on disk pending retry PASS',flush=True)
    # Crash all four servers with a pending record, then recover with no client
    # mounted: GC must reconstruct context rather than falsely ACK absence.
    unmount();stop(crash=True)
    blocked.parent.chmod(0o700);blocked.chmod(0o600);blocked=None
    start()
    wait(lambda:not files(ino),'restart without client reclaims pending fragments')
    wait(drained,'leader reap and fragment queues drain')
    mount_client()
    assert hashlib.sha256(sentinel.read_bytes()).hexdigest()==expected
    assert (mount/'peer-fold').read_bytes()==model
    print('cold two-peer fold and live-base reads after GC PASS',flush=True)
    os.unlink(mount/'peer-fold')
    wait(lambda:not files(fino),'fold fragments physically reclaimed')
    # Reclamation must retain its durable ledger when one configured member
    # is unavailable, even if the remaining three can still form a quorum.
    offline,fd,offline_ino=create('offline-member');os.close(fd)
    servers[3].kill();servers[3].wait()
    os.unlink(offline);time.sleep(3)
    remaining=[node for i,node in enumerate(status_available()) if i<3]
    assert any(group.get('reap_seen',0)>0 for node in remaining for group in node[2:])
    print('offline fourth member retains reap ledger PASS',flush=True)
    unmount();stop(crash=True);start()
    wait(lambda:not files(offline_ino),'rejoined member completes reclamation')
    wait(drained,'queues drain after member rejoins')
    mount_client()
    os.unlink(sentinel)
    wait(lambda:not files(sino),'sentinel physically reclaimed')
    wait(drained,'all fixture GC records retired')
    if a.posix:
        posix_checkpoint = totals(status())
        run(['python3',source/'tests/posix/posix_suite.py',mount,'--jobs','4','--timeout-s','30','--results',work/'posix.tsv'],timeout=600)
        peer_log=open(work/'peer-fuse.log','ab');handles.append(peer_log)
        peer=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{a.port}','default',str(peer_mount),'-f'],env=env,stdout=peer_log,stderr=peer_log)
        wait(lambda:subprocess.run(['mountpoint','-q',str(peer_mount)]).returncode==0,'peer mounted',40)
        run(['python3',source/'tests/posix/posix_2client.py','--local',mount,peer_mount,'--parent','gc-posix-peer','--results',work/'posix2.tsv'],timeout=600)
        run([source/'scripts/client.sh','stop',peer_mount]);peer.wait(timeout=15);peer=None
        wait(drained,'POSIX server deletion queues drained',180)
        after_posix = totals(status())
        for metric in ('passes', 'removed_fragments', 'reclaimed_payload_bytes'):
            assert after_posix[metric] > posix_checkpoint[metric], (metric, posix_checkpoint, after_posix)
        (work/'posix-gc-effect.json').write_text(json.dumps({
            'before': posix_checkpoint, 'after': after_posix}, indent=2))
        print('POSIX deletes advance GC passes and physical removal counters PASS',flush=True)
    unmount();stop()
    start()
    # No user fragment remains, even on a restart. Physical scan is independent
    # of logical namespace disappearance and the asynchronous quota snapshot.
    wait(lambda:all(not [x for x in (root/'data/exports/1').glob('**/*') if x.is_file()] for root in data_roots),
         'physical inventory empty after restart',180)
    wait(drained,'restart orphan and GC queues drained',180)
    final=status()
    assert all(node[1]['data_usage_bytes']==0 for node in final),final
    # A delayed valid-sized PUT must not resurrect the retired inode after
    # restart. The wire uses native C headers inside its big-endian framing.
    for i in range(4):
        reply=valid_put(i,ino)
        assert len(reply) in (2,3) and reply[:2]==bytes([7,1]),reply
    assert not files(ino)
    print('restart quota zero and late PUT fenced on every member PASS',flush=True)
    (work/'final-status.json').write_text(json.dumps(final,indent=2))
    print('physical fragment inventory empty after restart PASS',flush=True)
    if memory_group:
        events=dict(line.split() for line in (memory_group/'memory.events').read_text().splitlines())
        assert all(int(events[key])==int(memory_before[key]) for key in ('oom','oom_kill')),events
        record={'limit_bytes':a.memory_limit_bytes,'peak_bytes':int((memory_group/'memory.peak').read_text()),'events_before':memory_before,'events_after':events,'scope':str(memory_group)}
        assert record['peak_bytes']<=a.memory_limit_bytes,record
        (work/'memory-envelope.json').write_text(json.dumps(record,indent=2))
        print('small-memory whole-fixture envelope '+json.dumps(record)+' PASS',flush=True)
    print('GC reclamation '+a.mode+' PASS',flush=True)
finally:
    for proc,target in extra_clients:
        subprocess.run(['fusermount3','-uz',str(target)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        proc.terminate()
        try:proc.wait(timeout=5)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
    if blocked and blocked.exists():
        blocked.parent.chmod(0o700);blocked.chmod(0o600)
    if client:
        subprocess.run(['fusermount3','-uz',str(mount)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        client.terminate()
        try:client.wait(timeout=5)
        except subprocess.TimeoutExpired:client.kill();client.wait()
    if peer:
        subprocess.run(['fusermount3','-uz',str(peer_mount)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        peer.terminate()
        try:peer.wait(timeout=5)
        except subprocess.TimeoutExpired:peer.kill();peer.wait()
    stop()
    for h in handles:h.close()
