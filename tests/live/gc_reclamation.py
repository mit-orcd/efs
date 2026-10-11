#!/usr/bin/env python3
"""Isolated four-node physical reclamation gate (Linux/FUSE; no existing stores).

Run on the NUC: python3 tests/live/gc_reclamation.py --mode direct
The two root parents must already exist. Only unique harness-owned directories
and processes are used; failed fixtures/logs are retained for diagnosis.
"""
import argparse
import errno
import ctypes
import concurrent.futures
import struct
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import shutil
import threading
import socket
import subprocess
import select
import tempfile
from raft_tail import commands as raft_commands
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--peer-mkdir-race-only',action='store_true',help='64 competing peer mkdir races in an isolated cluster, then cold namespace verification')
p.add_argument('--peer-open-contention-only',action='store_true',help='twenty concurrent append/OPEN races, full peer POSIX, exact cold bytes and physical cleanup only')
p.add_argument('--append-truncate-only',action='store_true',help='run eight peer append/truncate races, a concurrent append handoff check, cold-remount repeats and physical cleanup only')
p.add_argument('--mount-sync',action='store_true',help='run only mount-wide sync acceptance: ordinary writes, peer-before-close, cold bytes and deletion')
p.add_argument('--mode', choices=('buffered', 'direct'), required=True)
p.add_argument('--transport', choices=('tcp', 'rdma'), default='tcp',
               help='TCP by default; use RDMA only for transport-specific gates')
p.add_argument('--root', action='append', default=[])
p.add_argument('--mount', type=Path, help='existing empty, unmounted path allowed by host FUSE policy')
p.add_argument('--peer-mount', type=Path, help='existing empty, unmounted second policy-allowed mount')
p.add_argument('--posix', action='store_true')
p.add_argument('--posix-jobs', type=int, default=4, help='parallel POSIX cases (default: 4); each case retains its own concurrent writers')
p.add_argument('--storage-session-fence-fault',action='store_true',help='private certificate fsync EIO: revocation must remain incomplete, then recover and survive cold restart; requires --guarded-leases')
p.add_argument('--continue-posix-failure','--continue-regression-failure',dest='continue_posix_failure',action='store_true',help='retain POSIX/peer failure, run independent later gates, then exit unsuccessfully')
p.add_argument('--tiny-sustained-large-mib-s',type=float,default=0,help='0=unpaced; otherwise 0..64 MiB/s per large writer in sustained gate')
p.add_argument('--tiny-overload',action='store_true',help='retain refused-I/O verdicts and cold-check acknowledged bytes in sustained gate')
p.add_argument('--replication-pressure',action='store_true',help='test-only 100-ms follower log-sync delay during seconds 30..60 of sustained load, then real peer catch-up')
p.add_argument('--tiny-sustained-seconds',type=int,default=0,help='1..1800 seconds of bounded tiny/large churn; cold exact survivors and physical reclaim, then exit')
p.add_argument('--tiny-mixed',action='store_true',help='bounded large-writer overlap with 512 tiny CREATE/close operations, cold exact bytes and physical deletion')
p.add_argument('--tiny-create-only',action='store_true',help='finite eight-worker tiny CREATE/write/close benchmark with exact cold bytes and physical reclaim, without large writers')
p.add_argument('--tiny-mixed-only',action='store_true',help='run only the mixed-load cold-read/reclamation gate; implies --tiny-mixed')
p.add_argument('--tiny-mixed-files',type=int,default=512,help='8..8192 tiny files, divisible by eight; use 4096 for longer mixed-load timing')
p.add_argument('--fixture-quota',choices=('0','1G','4G','16G'),help='explicit per-daemon private quota; default preserves each gate existing quota')
p.add_argument('--session-append',action='store_true',help='bounded revoked-session append holes, cold half-completion and exact epoch resume')
p.add_argument('--session-cleanup',action='store_true',help='staged bounded lease/lock cleanup, global barrier, exact replay and cold physical reclaim')
p.add_argument('--deferred-close',action='store_true',help='staged identified close, 64 cross-group OPEN appends, crash/resume, exact holes and bounded retirement')
p.add_argument('--typed-writeback',action='store_true',help='actual typed cache bytes, publication/retirement lost ACK and post-snapshot peer preservation')
p.add_argument('--guarded-leases',action='store_true',help='sequenced lease wire reorder, lost reply probe, cold replay and last-close reclaim')
p.add_argument('--put-barrier-fault',action='store_true',help='test-only server fragment fsync refusal, unpublished state, retained-body recovery and cold bytes')
p.add_argument('--create-adopt-negative',action='store_true',help='force a real stale FileID combined-adoption refusal, retain original CREATE token cleanup and verify name reuse cold; requires fault build')
p.add_argument('--create-adopt-reply-loss',action='store_true',help='hide one committed combined CREATE adoption reply; failed descriptor cleanup, name reuse and cold bytes; requires fault build')
p.add_argument('--create-live-retry',action='store_true',help='actual FUSE CREATE retries consumed committed reply then BUSY using one identity')
p.add_argument('--create-negative-replay',action='store_true',help='ordinary CREATE EEXIST identity survives unlink, cold restart and name reuse')
p.add_argument('--unique-create',action='store_true',help='16-worker unique O_EXCL, genuine EEXIST and name reuse, then cold exact bytes/reclamation')
p.add_argument('--create-lease',action='store_true',help='initial CREATE lease atomicity on local and first-use cross-group hashed lanes, cold replay/unlink/close')
p.add_argument('--fuse-reply-failure',action='store_true',help='reject CREATE/OPEN descriptor replies and prove local handle/server lease cleanup')
p.add_argument('--tiny-churn',action='store_true',help='tiny/full-fragment fsynced create/overwrite/delete/recreate, cold exact generations and physical reclamation')
p.add_argument('--wal-tail-recovery',action='store_true',help='repair private WAL invalid tails, append acknowledged data, then verify another cold restart')
p.add_argument('--sync-write-fault',action='store_true',help='actual O_SYNC/O_DSYNC callback failure retains bytes for later fsync recovery')
p.add_argument('--sync-writes',action='store_true',help='O_SYNC/O_DSYNC publication before write replies, including queued append across lane groups')
p.add_argument('--append-sync-boundary',action='store_true',help='append/fsync across lane groups, peer visibility and cold crash bytes')
p.add_argument('--append-replay-race', action='store_true', help='32 identical concurrent APPEND RPCs allocate once')
p.add_argument('--fresh-namespace',action='store_true',help='six first-mutation namespace operations on fresh cold mounts with no prior file OPEN')
p.add_argument('--append-burst-only',action='store_true',help='only exact APPEND burst, peer/cold bytes and physical cleanup')
p.add_argument('--append-burst',type=int,default=0,help='100..20000 unique 4-KiB O_APPEND writes, final fsync, exact peer/cold blocks')
p.add_argument('--append-audit', action='store_true', help='mixed append descriptions during physical GC, then exact cold records')
p.add_argument('--integrity', action='store_true')
p.add_argument('--namespace', action='store_true')
p.add_argument('--namespace-boundary', action='store_true')
p.add_argument('--namespace-boundary-fault',action='store_true',help='kill the 64-lane replacement coordinator at prepared and committed barriers')
p.add_argument('--namespace-cycles',action='store_true',help='race opposite directory moves using captured inode parents')
p.add_argument('--mount-label', default='default')
p.add_argument('--workers', action='store_true')
p.add_argument('--pressure', action='store_true')
p.add_argument('--production-budgets',action='store_true',help='exercise default 256 MiB body and 64 MiB drain budgets')
p.add_argument('--drain-pressure',action='store_true',help='physically exhaust recovery scratch while publication is withheld')
p.add_argument('--memory-limit-bytes',type=int,help='require an inherited cgroup memory ceiling and verify no OOM events')
p.add_argument('--publication-pressure',action='store_true',help='retain failed REPORT bodies until bounded admission refuses, then recover')
p.add_argument('--namespace-max',action='store_true',help='exact ancestry limit, over-limit refusal and restart recovery')
p.add_argument('--namespace-max-fault',action='store_true',help='kill exact 144-participant namespace coordinator before/after COMMIT')
p.add_argument('--stop-dirty-recovery',action='store_true',help='retain a new partial dirty body through failed fsync and cleanly drain after restart')
p.add_argument('--shutdown-mutations',action='store_true',help='gracefully stop a metadata leader during fsynced file creation; verify acknowledged bytes after restart')
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
p.add_argument('--applied-checkpoint-replay-only',action='store_true',help='retain follower applied markers, crash private fixture and verify durable metadata replay')
p.add_argument('--kv-wal-persistence-only',action='store_true',help='refuse follower KV WAL fsync; preserve periodic/shutdown checkpoints and cold replay')
p.add_argument('--transaction-fault-seed',action='store_true',help='prefer the client seed as fault follower for authority redirect validation')
p.add_argument('--transaction-apply-retry',action='store_true',help='fault follower transaction persistence, retain commit/apply lag, retry and cold namespace bytes')
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
p.add_argument('--gc-reap-files',type=int,choices=(256,1024),help='measure small-file reaping and settled metadata latency')
p.add_argument('--idle-seconds',type=int,default=0,help='measure live-table idle GC, process CPU and Raft commits')
p.add_argument('--full-node',action='store_true',help='one node quota exhausted while three fragment targets remain writable')
p.add_argument('--port', type=int, default=20190)
a = p.parse_args()
if not 0 <= a.tiny_sustained_large_mib_s <= 64:p.error('--tiny-sustained-large-mib-s must be 0..64')
if not 0 <= a.tiny_sustained_seconds <= 1800:p.error('--tiny-sustained-seconds must be 0..1800')
if a.append_burst_only and not a.append_burst:p.error('--append-burst-only requires --append-burst')
if a.append_burst and not 100<=a.append_burst<=20000:p.error('--append-burst must be 100..20000')
if a.posix_jobs < 1:
    p.error("--posix-jobs must be positive")
if a.mount_sync:a.sync_writes=True
if not 8 <= a.tiny_mixed_files <= 8192 or a.tiny_mixed_files % 8:
    p.error("--tiny-mixed-files must be 8..8192 and divisible by eight")
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
mount = a.mount.resolve() if a.mount else work/'mnt'
if a.mount:
    assert mount.is_dir() and not os.path.ismount(mount) and not any(mount.iterdir()), 'mount path must be empty and unmounted'
else:mount.mkdir()
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
env = dict(os.environ, EFS_MD_RAFT_N='4', EFS_TRANSPORT=a.transport)
if a.replication_pressure:
    assert a.tiny_sustained_seconds>=120, 'pressure requires a sustained recovery window'
    assert not (a.storage_session_fence_fault or a.put_barrier_fault or a.kv_wal_persistence_only or a.compactor_pressure)
    run_preload=work/'raft-slow-sync.so'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',source/'tests/live/raft_slow_sync.c','-ldl','-o',run_preload],check=True)
    env.update(LD_PRELOAD=str(run_preload),EFS_TEST_RAFT_SLOW_FILE=str(work/'raft-slow-armed'))
if a.storage_session_fence_fault:
    assert a.guarded_leases and not (a.put_barrier_fault or a.kv_wal_persistence_only or a.compactor_pressure), 'certificate fault must own its preload'
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-shared','-fPIC',source/'tests/live/storage_session_fence_fault.c','-ldl','-o',work/'put-session-fault.so'],check=True)
    env['EFS_TEST_PUT_FENCE_FAULT_FILE']=str(work/'put-fence-armed')

if a.transport == 'rdma':
    device = env.get('EFS_RDMA_DEV', 'rxe0')
    assert (Path('/sys/class/infiniband')/device).is_dir(), 'RDMA device absent; gate not run'
    env['EFS_RDMA_DEV'] = device
if a.gc_bulk_gib:
    assert not a.idle_seconds;env['EFS_GC_DBG']='1'
if a.ior_binary or a.append_sync_boundary or a.sync_writes or a.stop_dirty_recovery or a.sync_write_fault:env.update(EFS_DCACHE_TRACE='1',EFS_REPORT_DBG='1')
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
if a.namespace_max_fault:
    assert a.namespace_max and a.server_binary
if a.namespace_boundary_fault or a.namespace_max_fault:
    if a.namespace_boundary_fault:assert a.namespace_boundary and a.namespace and a.server_binary
    env['EFS_FAULT_TXN_BARRIER']=str(work/'txn-barrier')
servers=[]
client=None
peer=None
peer_mount=a.peer_mount.resolve() if a.peer_mount else work/'peer'
if a.peer_mount:
    assert peer_mount != mount and peer_mount.is_dir() and not os.path.ismount(peer_mount) and not any(peer_mount.iterdir()), 'peer mount must be distinct, empty and unmounted'
else:peer_mount.mkdir()
handles=[]
extra_clients=[]
blocked=None
posix_failure=None

def run(args, **kwargs):
    try:
        return subprocess.run([str(x) for x in args], check=True, timeout=kwargs.pop("timeout",60), env=kwargs.pop("env",env), **kwargs)
    except subprocess.CalledProcessError as error:
        # Captured helper assertions must remain visible in saved gate evidence.
        if error.stdout: print(error.stdout,flush=True)
        if error.stderr: print(error.stderr,flush=True)
        raise
def status_available():
    return status(3)

def session_admission(args, busy, timeout=60):
    """Retry one unchanged administrative admission only on explicit BUSY."""
    deadline=time.monotonic()+timeout
    while True:
        remaining=deadline-time.monotonic()
        if remaining<=0:raise subprocess.TimeoutExpired(args,timeout)
        result=subprocess.run(list(map(str,args)),env=env,capture_output=True,
                              text=True,timeout=remaining)
        if result.stdout:print(result.stdout,flush=True)
        if result.stderr:print(result.stderr,flush=True)
        if result.returncode==0:return result
        if not re.search(r'\brc='+str(busy)+r'\b',result.stdout):
            raise subprocess.CalledProcessError(result.returncode,args,
                                                result.stdout,result.stderr)
        time.sleep(min(.05,max(0,deadline-time.monotonic())))

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

def start(prefer_single_host=False):
    # Follower-persistence gates fault n3, which hosts both groups. Start its
    # other voters first and allow them to elect before n3 joins; otherwise
    # startup races can make the injected node a leader and invalidate the gate.
    follower_fault = a.kv_wal_persistence_only or a.applied_checkpoint_replay_only
    order = (0, 1, 3, 2) if follower_fault else range(len(roots))
    for i in order:
        if follower_fault and i == 2:
            time.sleep(3)
        root = roots[i]
        root.mkdir(exist_ok=True)
        log=open(work/f'n{i+1}.log','ab');handles.append(log)
        cmd=[a.server_binary or source/'efsd','--node-id',str(i+1),'--addr','127.0.0.1','--port',str(a.port+i),'--storage',root,'--quota',a.fixture_quota or ('16G' if (a.ior_binary or a.gc_bulk_gib) else ('4G' if a.compactor_pressure else '1G')),'--'+('direct-io' if a.mode=='direct' else 'no-direct-io')]
        if a.fixture_quota == '0':
            quota_arg = cmd.index('--quota');del cmd[quota_arg:quota_arg+2]
        if a.storage_roots==2:
            first=root/'storage0';second=root/'storage1'
            first.mkdir(exist_ok=True);second.mkdir(exist_ok=True)
            cmd[cmd.index(root)]=first
            cmd += ['--storage',second]
        if i:cmd += ['--join',f'127.0.0.1:{a.port}']
        node_env=dict(env)
        if a.storage_session_fence_fault:node_env['LD_PRELOAD']=str(work/'put-session-fault.so')
        if a.put_barrier_fault:
            node_env.update(LD_PRELOAD=str(work/'store-barrier-fault.so'),
                            EFS_TEST_STORE_BARRIER_FILE=str(work/'put-barrier-fault'),
                            EFS_TEST_STORE_NODE=str(i+1))
        if a.applied_checkpoint_replay_only and i==2:
            node_env.update(LD_PRELOAD=str(work/'applied-checkpoint-fault.so'),
                            EFS_TEST_APPLIED_FAULT=str(work/'applied-marker-fault'))
        if a.kv_wal_persistence_only and i==2:
            node_env.update(LD_PRELOAD=str(work/'kv-wal-fault.so'),
                            EFS_TEST_KV_WAL_FAULT=str(work/'kv-wal-fault'),
                            EFS_TEST_KV_CHECKPOINT=str(work/'kv-checkpoint-before-fault.bin'))
        if a.transaction_apply_retry:
            assert a.server_binary,'transaction retry gate requires an explicitly selected fault build'
            node_env['EFS_FAULT_TXN_APPLY_FILE']=str(work/f'transaction-apply-fault-{i+1}')
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
        if prefer_single_host and i==0:time.sleep(2)
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
    client_env=dict(env)
    if a.create_adopt_negative:
        client_env.update(EFS_FAULT_CREATE_ADOPT_NEGATIVE_FILE=str(work/'create-adopt-negative-fault'),EFS_OPID_TRACE='1')
    if a.create_adopt_reply_loss:
        client_env['EFS_FAULT_CREATE_ADOPT_REPLY_FILE']=str(work/'create-adopt-reply-fault')
    if a.create_live_retry:
        client_env['EFS_FAULT_CREATE_RETRY_FILE']=str(work/'create-retry-fault')
    if a.fuse_reply_failure:
        from fuse_reply_failure import prepare
        client_env=prepare(work,client_env)
    client=subprocess.Popen([str(a.client_binary or source/'efs-fuse'),f'127.0.0.1:{a.port}',a.mount_label,str(mount),'-f',*(['-o','sync'] if a.mount_sync else []),*(['-o','max_threads=16,max_idle_threads=1'] if a.workers else [])],env=client_env,stdout=log,stderr=log)
    def ready():
        if client.poll() is not None:raise AssertionError('FUSE exited')
        if subprocess.run(['mountpoint','-q',str(mount)]).returncode:return False
        return subprocess.run(['timeout','2','stat',str(mount)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0
    wait(ready,'client serving',40)
    if a.fixture_quota == '0':
        capacity = os.statvfs(mount)
        assert capacity.f_blocks > 0 and capacity.f_bavail > 0, capacity
        print('unlimited-quota filesystem capacity/free bytes PASS',flush=True)
    if a.mount_sync:
        options=subprocess.check_output(['findmnt','-rn','-o','OPTIONS','--target',str(mount)],text=True).strip().split(',')
        assert 'sync' in options,('mount sync flag absent',options)
    transport_proof([*servers,client])

def transport_proof(processes):
    if a.transport == 'rdma':
        # Fail instead of silently accepting a TCP fallback. Check the active
        # clients and servers, including each cold restart in this harness.
        proof=[]
        for proc in processes:
            devices=[]
            for fd in (Path('/proc')/str(proc.pid)/'fd').iterdir():
                try:
                    target=os.readlink(fd)
                except FileNotFoundError:
                    continue
                if '/dev/infiniband/uverbs' in target:
                    devices.append(target)
            assert devices, ('RDMA process has no verbs device',proc.pid)
            proof.append({'pid':proc.pid,'verbs_devices':sorted(set(devices))})
        with (work/'rdma-runtime.jsonl').open('a') as out:
            out.write(json.dumps(proof)+'\n')

def unmount(timeout=15):
    global client
    # mountpoint stats the FUSE root. During recovery GETATTR can fail even
    # though the kernel still lists this mount and its client remains alive.
    # The stop script checks /proc/mounts and process/control ownership without
    # depending on successful metadata RPCs; do not skip its drain in that case.
    if client or subprocess.run(['mountpoint','-q',str(mount)]).returncode==0:
        run([source/'scripts/client.sh','stop',mount])
    if client:
        client.wait(timeout=timeout);client=None

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

def chunk_state(ino, allow_absent=False):
    # Query all reachable replicas until a current authoritative reply exists.
    # BUSY/NOTPRIMARY are not a missing chunk or a zero-filled image.
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        for node in range(4):
            reply=subprocess.run([str(source/'efs-mgmt'),'raft-getchunks',f'127.0.0.1:{a.port+node}',str(ino)],env=env,capture_output=True,text=True,timeout=10)
            if reply.returncode==0 and 'status=0 ' in reply.stdout:
                if allow_absent and re.search(r'count=0\b',reply.stdout):
                    return dict(base_gen=0,spans=0,seq=0,live_spans=0,tombstones=0)
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
    if a.put_barrier_fault:
        run(['cc','-Wall','-Wextra','-Werror','-shared','-fPIC',source/'tests/live/store_barrier_fault.c','-ldl','-o',work/'store-barrier-fault.so'])
    if a.applied_checkpoint_replay_only:
        run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror',source/'tests/live/applied_checkpoint_fault.c','-ldl','-o',work/'applied-checkpoint-fault.so'])
    if a.kv_wal_persistence_only:
        run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror',source/'tests/live/kv_wal_fault.c','-ldl','-o',work/'kv-wal-fault.so'])
    start()
    run([source/'efs-mgmt','raft-mkfs',f'127.0.0.1:{a.port}'])
    mount_client()
    if a.fresh_namespace:
        fresh=mount/'fresh-namespace';fresh.mkdir()
        for name in ('unlink','rename','link'):
            with (fresh/name).open('xb',buffering=0) as fd:
                assert fd.write(b'fresh-owner:'+name.encode())==12+len(name)
                os.fsync(fd.fileno())
        (fresh/'rmdir').mkdir()
        checks=[]
        for action in ('unlink','rename','rmdir','mkdir','create','link'):
            unmount();stop();start();mount_client()
            # Read-only stat/LOOKUP can precede the mutation. A file OPEN
            # must not silently bootstrap the global session in this gate.
            target=fresh/action
            started=time.monotonic()
            if action=='unlink':target.unlink();assert not target.exists()
            elif action=='rename':
                target.rename(fresh/'renamed')
                assert (fresh/'renamed').read_bytes()==b'fresh-owner:rename'
            elif action=='rmdir':target.rmdir();assert not target.exists()
            elif action=='mkdir':target.mkdir();assert target.is_dir()
            elif action=='create':
                with target.open('xb',buffering=0) as fd:
                    assert fd.write(b'new-owner')==9;os.fsync(fd.fileno())
                assert target.read_bytes()==b'new-owner'
            else:
                os.link(target,fresh/'linked');assert target.stat().st_nlink==2
                assert (fresh/'linked').read_bytes()==b'fresh-owner:link'
            checks.append(dict(first_mutation=action,elapsed_s=time.monotonic()-started,passed=True))
            (work/'fresh-namespace.json').write_text(json.dumps(checks,indent=2))
        shutil.rmtree(fresh);wait(drained,'fresh namespace cold first-mutation physical cleanup',180)
        print('six cold mounts first CREATE/MKDIR/UNLINK/RMDIR/RENAME/LINK without prior OPEN PASS',flush=True)
    if a.append_burst_only:
        import append_burst
        peer_log=open(work/'peer-fuse.log','ab');handles.append(peer_log)
        peer=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{a.port}',a.mount_label,str(peer_mount),'-f'],env=env,stdout=peer_log,stderr=peer_log)
        wait(lambda:os.path.ismount(peer_mount),'append burst peer mounted',40)
        burst_path,burst_report=append_burst.run(mount,peer_mount,a.append_burst,work)
        burst_ino=burst_path.stat().st_ino
        run([source/'scripts/client.sh','stop',peer_mount]);peer.wait(timeout=15);peer=None
        unmount();stop();start();mount_client()
        burst_report['cold_sha256']=append_burst.verify(burst_path,a.append_burst)
        assert burst_report['cold_sha256']==burst_report['warm_primary_sha256']
        burst_path.unlink()
        wait(lambda:not files(burst_ino),'append burst physical fragments empty',180)
        wait(drained,'append burst GC drained',180)
        wait(lambda:totals(status())['data_usage_bytes']==0,'append burst quota zero',180)
        (work/'append-burst.json').write_text(json.dumps(burst_report,indent=2))
        print('append burst exact peer/cold blocks, final fsync and zero physical/quota PASS',flush=True)
        raise SystemExit(0)
    if a.peer_mkdir_race_only:
        peer_log=open(work/'peer-fuse.log','ab');handles.append(peer_log)
        peer=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{a.port}',a.mount_label,str(peer_mount),'-f'],env=env,stdout=peer_log,stderr=peer_log)
        wait(lambda:subprocess.run(['mountpoint','-q',str(peer_mount)]).returncode==0,'peer mounted',40)
        transport_proof([peer])
        for iteration in range(8):
            run(['python3',source/'tests/posix/posix_2client.py','--local',mount,peer_mount,
                 '--parent',f'mkdir-race-{iteration:02d}','--filter','peer_mkdir_exclusive_race','--keep',
                 '--results',work/f'mkdir-race-{iteration:02d}.tsv'],timeout=180)
        run([source/'scripts/client.sh','stop',peer_mount]);peer.wait(timeout=15);peer=None
        unmount();stop();start();mount_client()
        for iteration in range(8):
            parent=mount/f'mkdir-race-{iteration:02d}/peer_mkdir_exclusive_race'
            for trial in range(8):assert (parent/f'raced-{trial}').is_dir()
            shutil.rmtree(mount/f'mkdir-race-{iteration:02d}')
        wait(drained,'peer mkdir physical reclamation',180)
        (work/'peer-mkdir-race.json').write_text(json.dumps({'races':64,'cold_directories':64},indent=2))
        print('64 competing peer MKDIR races, exact verdicts and cold directories PASS',flush=True)
        raise SystemExit(0)
    if a.peer_open_contention_only:
        peer_log=open(work/'peer-fuse.log','ab');handles.append(peer_log)
        peer=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{a.port}',a.mount_label,str(peer_mount),'-f'],env=env,stdout=peer_log,stderr=peer_log)
        wait(lambda:subprocess.run(['mountpoint','-q',str(peer_mount)]).returncode==0,'peer mounted',40)
        transport_proof([peer])
        for iteration in range(20):
            run(['python3',source/'tests/posix/posix_2client.py','--local',mount,peer_mount,
                 '--parent','open-contention','--filter','peer_concurrent_append','--keep',
                 '--results',work/f'append-open-{iteration:02d}.tsv'],timeout=180)
        path=mount/'open-contention/peer_concurrent_append/f'
        with path.open('rb') as fd:os.fsync(fd.fileno())
        expected=path.read_bytes();assert len(expected.splitlines())==800
        run(['python3',source/'tests/posix/posix_2client.py','--local',mount,peer_mount,
             '--parent','gc-posix-peer','--results',work/'posix2.tsv'],timeout=600)
        run([source/'scripts/client.sh','stop',peer_mount]);peer.wait(timeout=15);peer=None
        unmount();stop();start();mount_client()
        assert path.read_bytes()==expected,'concurrent append cold bytes changed'
        record={'iterations':20,'records_per_iteration':800,'cold_bytes':len(expected),
                'sha256':hashlib.sha256(expected).hexdigest()}
        (work/'peer-open-contention.json').write_text(json.dumps(record,indent=2))
        shutil.rmtree(mount/'open-contention')
        wait(drained,'peer OPEN contention physical reclamation',180)
        print('twenty concurrent append/OPEN races, full peer POSIX and exact cold bytes PASS',flush=True)
        raise SystemExit(0)
    if a.applied_checkpoint_replay_only:
        from applied_checkpoint_replay import exercise
        exercise(a,work,roots,mount,run,start,stop,mount_client,unmount,wait,drained)
        raise SystemExit(0)
    if a.kv_wal_persistence_only:
        from kv_wal_persistence import exercise
        exercise(a,work,roots,mount,run,start,stop,mount_client,unmount,wait,drained)
        raise SystemExit(0)
    if a.put_barrier_fault:
        from put_barrier_fault import exercise as fail_put_barrier, verify_and_delete as verify_put_barrier
        barrier=fail_put_barrier(mount,work/'put-barrier-fault',lambda ino:chunk_state(ino,allow_absent=True))
        assert any('test-store-barrier: refusing fragment' in (work/f'n{i+1}.log').read_text() for i in range(4))
        unmount();stop(crash=True);start();mount_client()
        verify_put_barrier(mount)
        (work/'put-barrier-fault').unlink(missing_ok=True)
        wait(drained,'failed PUT barrier recovery cold bytes and deletion drained',180)
        (work/'put-barrier-fault.json').write_text(json.dumps(barrier,indent=2))
        print('failed server PUT barrier: no publication ACK, retained bytes, recovery and exact cold bytes PASS',flush=True)
    if a.create_live_retry:
        marker=work/'create-retry-fault';marker.touch()
        path=mount/'w58-live-retry';payload=b'W58 committed reply lost, BUSY replay, same identity'
        try:
            with path.open('xb',buffering=0) as out:
                assert out.write(payload)==len(payload)
                os.fsync(out.fileno())
            ino=path.stat().st_ino
            try:os.open(path,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600)
            except FileExistsError:pass
            else:raise AssertionError('genuine O_EXCL conflict accepted')
        finally:marker.unlink(missing_ok=True)
        log=(work/'fuse.log').read_text()
        stages=re.findall(r'test-create-retry: stage=(\d+) seq=(\d+) ino=(\d+) identical=1',log)
        assert len(stages)==3 and [int(x[0]) for x in stages]==[0,1,2],stages
        assert len({x[1:] for x in stages})==1 and int(stages[0][2])==ino,stages
        unmount();stop(crash=True);start();mount_client()
        assert path.read_bytes()==payload
        path.unlink()
        wait(drained,'live CREATE ambiguous/BUSY retry cold deletion drained',180)
        (work/'create-live-retry.json').write_text(json.dumps({
            'stages':stages,'original_durable_inode':ino,'cold_bytes':True,
            'genuine_exclusive_conflict':True},indent=2))
        print('actual FUSE CREATE lost committed reply, BUSY replay, same opid/inode and cold bytes PASS',flush=True)
    adoption_faults=(['reply-loss'] if a.create_adopt_reply_loss else [])+(['negative'] if a.create_adopt_negative else [])
    for adoption_fault in adoption_faults:
        marker=work/('create-adopt-reply-fault' if adoption_fault=='reply-loss' else 'create-adopt-negative-fault')
        marker.touch();path=mount/('w94-adopt-'+adoption_fault)
        try:
            fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600)
        except OSError as error:
            assert error.errno==errno.EIO,error
        else:
            os.close(fd);raise AssertionError('combined adoption '+adoption_fault+' not injected')
        assert not marker.exists(),'fault build did not consume adoption marker'
        ino=path.stat().st_ino;log=(work/'fuse.log').read_text()
        pattern=(r'test-create-adopt-reply-loss seq=(\d+) ino=(\d+) known=1 hidden=1'
                 if adoption_fault=='reply-loss' else
                 r'opid-create-adopt: rejected seq=(\d+) ino=(\d+) rc=-14 terminal=1')
        matches=re.findall(pattern,log)
        assert len(matches)==1 and int(matches[0][1])==ino,matches
        if adoption_fault=='negative':
            forced=re.findall(r'test-create-adopt-negative-request ino=(\d+) generation=(\d+) original=(\d+)',log)
            assert len(forced)==1 and int(forced[0][0])==ino and forced[0][1]!=forced[0][2],forced
        path.unlink()
        def inode_gone():
            for node in range(4):
                reply=subprocess.run([str(source/'efs-mgmt'),'raft-getattr',
                    f'127.0.0.1:{a.port+node}',str(ino)],env=env,capture_output=True,text=True,timeout=10)
                if reply.returncode==0 and re.search(r'status=1\b',reply.stdout):return True
            return False
        wait(inode_gone,'combined '+adoption_fault+' descriptor cleanup releases original inode',90)
        payload=('W94 recreated after combined '+adoption_fault).encode()
        with path.open('xb',buffering=0) as out:
            assert out.write(payload)==len(payload);os.fsync(out.fileno())
        reused_ino=path.stat().st_ino;assert reused_ino!=ino
        unmount();stop(crash=True);start();mount_client()
        assert path.read_bytes()==payload
        path.unlink();wait(lambda:not files(reused_ino),'combined '+adoption_fault+' name reuse cold bytes reclaimed',180)
        (work/('create-adopt-'+adoption_fault+'.json')).write_text(json.dumps(dict(
            fault=adoption_fault,edge_sequence=int(matches[0][0]),original_inode=ino,
            initial_errno=errno.EIO,descriptor_acknowledged=False,original_inode_reclaimed=True,
            recreated_inode=reused_ino,cold_bytes_verified=True),indent=2))
        print('actual FUSE combined '+adoption_fault+': failed descriptor cleanup, name reuse and cold bytes PASS',flush=True)
    if a.create_negative_replay:
        helper=work/'create-negative-replay'
        run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),
             source/'tests/live/create_negative_replay.c',source/'libefs.a',
             '-pthread','-lm','-ldl','-libverbs','-o',helper])
        run([helper,a.port,'prepare'],timeout=60)
        unmount();stop(crash=True);start();mount_client()
        assert not (mount/'w58-negative').exists()
        run([helper,a.port,'replay'],timeout=60)
        assert not (mount/'w58-negative').exists()
        (work/'create-negative-replay.json').write_text(json.dumps({
            'ordinary_create':True,'original_verdict':'EEXIST',
            'crash_restart_after_unlink':True,'name_reuse_preserved':True},indent=2))
        print('ordinary CREATE original EEXIST, unlink, cold replay and name reuse PASS',flush=True)
    if a.unique_create:
        from unique_create import exercise as unique_create, verify_and_delete as verify_unique
        unique=unique_create(mount)
        unmount();stop(crash=True);start();mount_client()
        verify_unique(mount)
        wait(drained,'unique CREATE cold bytes and physical deletion drained',180)
        (work/'unique-create.json').write_text(json.dumps(unique,indent=2))
        print('16-worker unique CREATE, real EEXIST, name reuse and exact cold bytes PASS',flush=True)
    if a.append_truncate_only:
        peer_log=open(work/'peer-fuse.log','ab');handles.append(peer_log)
        peer=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{a.port}','default',str(peer_mount),'-f'],env=env,stdout=peer_log,stderr=peer_log)
        wait(lambda:subprocess.run(['mountpoint','-q',str(peer_mount)]).returncode==0,'peer mounted',40)
        for index in range(8):
            run(['python3',source/'tests/posix/posix_2client.py','--local',mount,peer_mount,
                 '--parent',f'append-truncate-{index}','--filter','peer_append_while_truncate',
                 '--results',work/f'append-truncate-{index}.tsv'],timeout=120)
            if index==3:
                run([source/'scripts/client.sh','stop',peer_mount]);peer.wait(timeout=15);peer=None
                unmount();stop();start();mount_client()
                peer=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{a.port}','default',str(peer_mount),'-f'],env=env,stdout=peer_log,stderr=peer_log)
                wait(lambda:subprocess.run(['mountpoint','-q',str(peer_mount)]).returncode==0,'cold peer mounted',40)
        run(['python3',source/'tests/posix/posix_2client.py','--local',mount,peer_mount,
             '--parent','append-handoff','--filter','peer_concurrent_append',
             '--results',work/'append-handoff.tsv'],timeout=240)
        run([source/'scripts/client.sh','stop',peer_mount]);peer.wait(timeout=15);peer=None
        wait(drained,'append/truncate physical deletion drained',180)
        (work/'append-truncate-verdict.json').write_text(json.dumps({'pass':True,'truncate_repeats':8,'concurrent_append_records':800,'cold_restart_after_repeat':4},indent=2))
        unmount();stop()
        print('eight peer append/truncate races, concurrent handoff and cold restart PASS',flush=True)
        raise SystemExit(0)
    if a.fuse_reply_failure:
        from fuse_reply_failure import exercise as reject_replies
        evidence=reject_replies(mount,work,source,a.port,env)
        (work/'fuse-reply-failure.json').write_text(json.dumps(evidence,indent=2))
        print('rejected CREATE/OPEN replies release handles and reclaim server inodes PASS',flush=True)
    if a.tiny_sustained_seconds:
        from tiny_sustained import exercise as sustained,verify_and_delete as verify_sustained
        pressure_done=threading.Event();pressure_errors=[];pressure_thread=None
        if a.replication_pressure:
            def pressure_phases():
                began=time.monotonic();slow_node=None
                def snapshot(stage):
                    rows=[run([source/'efs-mgmt','raft-status',f'127.0.0.1:{a.port+i}'],capture_output=True,text=True,timeout=10).stdout for i in range(4)]
                    with (work/'replication-pressure.jsonl').open('a') as out:
                        out.write(json.dumps(dict(stage=stage,seconds=time.monotonic()-began,nodes=rows,slow_node=slow_node))+'\n')
                    return rows
                try:
                    rows=snapshot('before')
                    leaders={i for i,row in enumerate(rows) if 'role=LEADER' in row}
                    candidates=[i for i in (2,3,0,1) if i not in leaders and 'hosted=1' in rows[i]]
                    assert candidates,rows
                    slow_node=candidates[0]+1
                    if pressure_done.wait(30):return
                    (work/'raft-slow-armed').write_text(str(slow_node)+'\n');snapshot('slow')
                    if pressure_done.wait(30):return
                    (work/'raft-slow-armed').unlink();snapshot('healed')
                    while not pressure_done.wait(30):snapshot('recovery')
                except BaseException as error:pressure_errors.append(error)
                finally:(work/'raft-slow-armed').unlink(missing_ok=True)
            pressure_thread=threading.Thread(target=pressure_phases)
            pressure_thread.start()
        try:
            record=sustained(mount,[client.pid,*[proc.pid for proc in servers]],
                             a.tiny_sustained_seconds,work/'tiny-sustained.json',a.tiny_sustained_large_mib_s,a.tiny_overload)
        finally:
            pressure_done.set()
            if pressure_thread:pressure_thread.join(timeout=45)
        if a.replication_pressure:
            assert not pressure_thread.is_alive() and not pressure_errors,pressure_errors
            # Freeze the actual leader cuts after acknowledged workload settles;
            # each hosted follower must reach those cuts before cold restart.
            def views():
                return [run([source/'efs-mgmt','raft-status',f'127.0.0.1:{a.port+i}'],capture_output=True,text=True,timeout=10).stdout for i in range(4)]
            before=views();cuts={}
            for row in before:
                for group,commit in re.findall(r'group (\d+) hosted=1 .*?commit=(\d+)',row):
                    group=int(group);cuts[group]=max(cuts.get(group,0),int(commit))
            assert set(cuts)=={0,2},cuts
            caught=[]
            def catchup():
                rows=views();found={0:0,2:0}
                for row in rows:
                    for group,applied in re.findall(r'group (\d+) hosted=1 .*?applied=(\d+)',row):
                        group=int(group);found[group]+=1
                        if int(applied)<cuts[group]:return False
                if min(found.values())<3:return False
                caught[:]=rows;return True
            wait(catchup,'both-group frozen leader cuts caught up after slow follower healed',90)
            (work/'replication-catchup.json').write_text(json.dumps(dict(cuts=cuts,before=before,after=caught),indent=2))
            slow_logs='\n'.join((work/f'n{i+1}.log').read_text() for i in range(4))
            assert 'slow-raft-sync armed' in slow_logs and 'slow-raft-sync healed' in slow_logs
            record['replication_catchup_verified']=True

        cold_start=time.monotonic()
        unmount();stop(crash=True);start();mount_client()
        if a.tiny_overload:
            from tiny_sustained import verify_overload_and_delete
            verify_overload_and_delete(mount,record)
        else: verify_sustained(mount,record['generations'])
        record.update(cold_verified=True,physical_inventory_empty=False,
                      cold_recovery_wall_s=time.monotonic()-cold_start)
        (work/'tiny-sustained.json').write_text(json.dumps(record,indent=2))
        drain_start=time.monotonic()
        try:
            wait(lambda:drained() and all(not any(path.is_file() for path in
                 (root/'data/exports/1').glob('**/*')) for root in data_roots),
                 'sustained cold bytes and physical deletion drained',180)
            record['physical_inventory_empty']=True
        finally:
            record.update(gc_drain_wall_s=time.monotonic()-drain_start,gc_drain_deadline_s=180)
            (work/'tiny-sustained.json').write_text(json.dumps(record,indent=2))
        if a.tiny_overload:
            # Pressure must recover without an operator clearing a sticky state.
            recovery = mount/'overload-recovery'
            body = hashlib.sha256(b'overload-recovered').digest()*1024
            with recovery.open('wb') as stream:
                assert stream.write(body)==len(body)
                stream.flush();os.fsync(stream.fileno())
            unmount();stop(crash=True);start();mount_client()
            assert recovery.read_bytes()==body
            recovery.unlink()
            wait(lambda:drained() and all(not any(path.is_file() for path in
                 (root/'data/exports/1').glob('**/*')) for root in data_roots),
                 'post-pressure cold write and physical deletion drained',180)
            record['post_pressure_cold_write_verified']=True
            (work/'tiny-sustained.json').write_text(json.dumps(record,indent=2))
        print(('overload acknowledged-byte recovery and physical reclamation PASS; refused I/O remains in verdict' if a.tiny_overload else 'sustained tiny/large warm generations, cold bytes and physical reclamation PASS'),flush=True)
        raise SystemExit(0)
    if a.tiny_churn:
        from tiny_churn import exercise as churn,verify_and_delete as verify_churn
        record=churn(mount)
        unmount();stop(crash=True);start();mount_client()
        verify_churn(mount)
        wait(drained,'tiny churn cold generations and physical deletion drained',180)
        (work/'tiny-churn.json').write_text(json.dumps(record,indent=2))
        print('tiny create/overwrite/delete/recreate: exact cold identities and physical reclamation PASS',flush=True)
    if a.tiny_mixed or a.tiny_mixed_only or a.tiny_create_only:
        from tiny_mixed import exercise,verify_and_delete
        large_workers=0 if a.tiny_create_only else 4
        mixed=exercise(mount,[client.pid,*[proc.pid for proc in servers]],a.tiny_mixed_files,large_workers)
        unmount();stop(crash=True);start();mount_client()
        verification_workers=8 if a.tiny_create_only else 1
        verify_and_delete(mount,a.tiny_mixed_files,large_workers,verification_workers)
        mixed['cold_verification_workers']=verification_workers
        # Sampled-zero counters can predate the final unlink. The early-exit
        # fixture must independently prove physical absence, as the ordinary
        # full fixture does after restart. Scan only when queues look empty.
        wait(lambda:drained() and all(not any(path.is_file() for path in
             (root/'data/exports/1').glob('**/*')) for root in data_roots),
             'mixed tiny/large cold bytes and physical deletion drained',180+a.tiny_mixed_files//16)
        mixed.update(cold_verified=True,physical_inventory_empty=True)
        (work/'tiny-mixed.json').write_text(json.dumps(mixed,indent=2))
        print(f'{a.tiny_mixed_files} tiny CREATE/close, large_workers={large_workers}; exact cold bytes and reclaim PASS',flush=True)
        if a.tiny_mixed_only or a.tiny_create_only:
            raise SystemExit(0)  # finally still drains all owned processes
    if a.session_cleanup:
        helper=work/'session-cleanup'
        run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),source/'tests/live/session_cleanup.c',source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',helper])
        directory=mount/'bounded-session-cleanup';directory.mkdir()
        paths=[]
        for i in range(13):
            path=directory/str(i)
            with path.open('wb') as f:
                f.write(bytes([i+1])*4096);f.flush();os.fsync(f.fileno())
            paths.append((path.stat().st_ino,path))
        paths.sort();inos=[ino for ino,path in paths];uuid=os.urandom(16).hex()
        records=[]
        def cleanup(stage):
            result=run([helper,stage,str(a.port),uuid,*map(str,inos)],capture_output=True,text=True,timeout=180).stdout
            records.append({'stage':stage,'result':result})
        cleanup('prepare')
        for ino,path in paths:path.unlink()
        directory.rmdir()
        assert all(files(ino) for ino in inos),'active cleanup dropped owned file'
        cleanup('first')
        wait(lambda:all(not files(ino) for ino in inos[:8]),'bounded first lease batch physical reclaim',180)
        assert all(files(ino) for ino in inos[8:]),'exact replay consumed next lease batch'
        unmount();stop(crash=True);start();mount_client()
        assert all(files(ino) for ino in inos[8:]),'cold replay lost remaining owners'
        # Resume the exact revoked epoch through the production management CLI.
        # BEGIN again would revoke another epoch and lose the original work identity.
        shard=inos[0] & 4095
        result=run([source/'efs-mgmt','raft-session','127.0.0.1:'+str(a.port),'cleanup-leases',uuid,'1',str(shard)],capture_output=True,text=True,timeout=180)
        assert 'rc=0' in result.stdout,result.stdout
        records.append({'stage':'management-resume','result':result.stdout})
        cleanup('finish') # Independently proves the lease/lock phase is now empty.
        wait(lambda:all(not files(ino) for ino in inos),'bounded session cold physical reclaim',180)
        inspector=work/'session-cleanup-inspect'
        run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),source/'tests/live/close_step.c',source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',inspector])
        time.sleep(12)
        unmount();stop()
        # Earlier phases have unrelated close obligations. Inspect every owned
        # inode on every stopped replica; global backlog remains diagnostic.
        retired=[run([inspector,'inspect',root/'mdraft/kv',str(ino)],capture_output=True,text=True).stdout for root in roots for ino in inos]
        assert all(text.startswith('markers=0 reservations=0 ') and 'open=0 ' in text and 'locks=0 ' in text and 'receipts=0 ' in text for text in retired),retired
        records.append({'stage':'offline-close-obligations-retired','result':retired})
        start();mount_client()
        (work/'session-cleanup.json').write_text(json.dumps({'inos':inos,'records':records},indent=2))
        print('bounded session lease/lock global barrier, exact replay, cold remaining owners and physical reclaim PASS',flush=True)
    if a.typed_writeback:
        typed=work/'client-publication-writeback'
        run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),source/'tests/live/client_publication_writeback.c',source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',typed])
        for group in ('cross','same'):
            typed_env={**env}
            if group=='same':typed_env['EFS_TYPED_SAME_GROUP']='1'
            typed_path=mount/('typed-writeback-'+group);typed_path.touch()
            typed_ino=typed_path.stat().st_ino
            typed_state=work/('typed-writeback-'+group+'-state.bin')
            typed_result=run([typed,str(a.port),str(typed_ino),typed_state],capture_output=True,text=True,timeout=180,env=typed_env).stdout
            (work/('typed-writeback-'+group+'-wire.txt')).write_text(typed_result)
            typed_ci=int(re.search(r'ci=(\d+)',typed_result).group(1))
            typed_group=int(re.search(r'group=(\d+)',typed_result).group(1))
            leaders=[]
            for node in range(4):
                raft=run([source/'efs-mgmt','raft-status',f'127.0.0.1:{a.port+node}'],capture_output=True,text=True).stdout
                if re.search(r'group '+str(typed_group)+r' .*role=LEADER',raft):leaders.append(node)
            assert len(leaders)==1,leaders
            dead=leaders[0];servers[dead].kill();servers[dead].wait(timeout=10)
            leader_replay=run([typed,str(a.port),str(typed_ino),typed_state,'receipt-only'],capture_output=True,text=True,timeout=180,env=typed_env).stdout
            (work/('typed-writeback-'+group+'-leader-replay.txt')).write_text(leader_replay)
            print('typed retired publication replay after actual lane leader death PASS',flush=True)
            unmount();stop(crash=True);start();mount_client()
            expected=bytearray([0x5a])*(128<<10);expected[4096:8192]=bytes([0xa7])*4096;expected[8192:12288]=bytes([0x33])*4096
            assert typed_path.stat().st_ino==typed_ino and typed_path.read_bytes()==bytes(typed_ci*(128<<10))+expected
            cold_replay=run([typed,str(a.port),str(typed_ino),typed_state,'verify'],capture_output=True,text=True,timeout=180,env=typed_env).stdout
            (work/('typed-writeback-'+group+'-cold-replay.txt')).write_text(cold_replay)
            typed_path.unlink();wait(lambda:not files(typed_ino),'typed writeback physical deletion',180)
            print('actual typed cache, lost ACKs, newer-range ownership, peer preservation and exact cold bytes PASS',flush=True)

    if a.guarded_leases:
        pending_helper=work/'client-pending-create'
        run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),source/'tests/live/client_pending_create.c',source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',pending_helper])
        pending_results=[]
        for hashed,adopt_before_crash in ((False,False),(True,False),(False,True),(True,True)):
            pending_dir=mount/('pending-create-hashed' if hashed else 'pending-create-wire');pending_dir.mkdir()
            if hashed:
                for operation in ('begin','finish'):
                    for node in range(4):
                        spread=subprocess.run([str(source/'efs-mgmt'),'raft-dir',f'127.0.0.1:{a.port+node}',str(pending_dir.stat().st_ino),operation],capture_output=True,text=True,timeout=30,env=env)
                        if spread.returncode==0 and 'status=0' in spread.stdout:break
                    else:raise AssertionError('pending CREATE spread failed: '+spread.stdout+spread.stderr)
            stage=('adopt-hashed' if hashed else 'adopt') if adopt_before_crash else ('create-hashed' if hashed else 'create')
            created_pending=run([pending_helper,str(a.port),str(pending_dir.stat().st_ino),stage],capture_output=True,text=True,timeout=90).stdout
            pending_uuid=created_pending.split('pending_uuid=')[1].split()[0]
            pending_name=created_pending.split('name=')[1].split()[0]
            pending_path=pending_dir/pending_name
            pending_before=pending_path.stat().st_ino
            unmount();stop(crash=True);start();mount_client()
            assert pending_path.stat().st_ino==pending_before,'pending CREATE lost across cold WAL replay'
            stage=('resume-adopt-hashed' if hashed else 'resume-adopt') if adopt_before_crash else ('resume-hashed' if hashed else 'resume')
            pending_reply=subprocess.run([str(pending_helper),str(a.port),str(pending_dir.stat().st_ino),stage,pending_uuid],capture_output=True,text=True,timeout=90)
            assert pending_reply.returncode==0,pending_reply.stderr
            pending_results.append(created_pending+pending_reply.stdout)
            pending_ino=pending_path.stat().st_ino
            pending_path.unlink();pending_dir.rmdir()
            wait(lambda:not files(pending_ino),'pending CREATE adopted/closed inode physical reclaim',180)
        (work/'pending-create-wire.txt').write_text(''.join(pending_results))
        retained_create=work/'client-retained-create'
        run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),source/'tests/live/client_retained_create.c',source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',retained_create])
        retained_dir=mount/'retained-create-request';retained_dir.mkdir()
        retained_results=[]
        for discard in (0,1):
            name='retained'+str(discard)
            retained_results.append(run([retained_create,str(a.port),str(retained_dir.stat().st_ino),name,str(discard)],capture_output=True,text=True,timeout=90).stdout)
            retained_path=retained_dir/name
            with retained_path.open('wb') as out:
                out.write(b'retained create ownership'*4096);out.flush();os.fsync(out.fileno())
            retained_ino=retained_path.stat().st_ino
            retained_path.unlink()
            wait(lambda:not files(retained_ino),'retained CREATE exact adoption and physical reclaim',180)
        retained_dir.rmdir()
        (work/'retained-create-wire.txt').write_text(''.join(retained_results))
        client_edge=work/'client-lease-edge'
        run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),source/'tests/live/client_lease_edge.c',source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',client_edge])
        edge_path=mount/'client-lease-transport';edge_body=b'client lease transport'*2048
        with edge_path.open('wb') as out:out.write(edge_body);out.flush();os.fsync(out.fileno())
        edge_ino=edge_path.stat().st_ino;edge_uuid=(os.urandom(8)+bytes(8)).hex();edge_results=[]
        many=work/'client-report-many-inodes';many_dir=mount/'report-many-inodes';many_dir.mkdir()
        many_paths=[many_dir/str(i) for i in range(40)]
        for path in many_paths:path.touch()
        many_input=work/'report-many-inodes.txt';many_inos=[path.stat().st_ino for path in many_paths]
        many_input.write_text(''.join(str(ino)+'\n' for ino in many_inos))
        run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),source/'tests/live/client_report_many_inodes.c',source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',many])
        many_result=run([many,str(a.port),many_input],capture_output=True,text=True,timeout=180).stdout
        many_rows=re.findall(r'ino=(\d+) ci=(\d+) byte=(\d+)',many_result);assert len(many_rows)==40,many_result
        (work/'report-many-inodes-wire.txt').write_text(many_result)
        unmount();stop(crash=True);start();mount_client()
        for path,ino,row in zip(many_paths,many_inos,many_rows):
            captured,ci,byte=map(int,row);assert captured==ino and path.stat().st_ino==ino
            data=path.read_bytes();assert data==bytes(ci*(128<<10))+bytes([byte])*(128<<10),('many-inode REPORT cold bytes',path,row)
            path.unlink()
        many_dir.rmdir();wait(lambda:all(not files(ino) for ino in many_inos),'40-inode fresh cross-group REPORT cold bytes and physical reclaim',180)

        report=work/'client-report-session';report_saved=work/'report-session-packet.bin'
        run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),source/'tests/live/client_report_session.c',source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',report])
        report_result=run([report,str(a.port),str(edge_ino),source/'efs-mgmt',report_saved],capture_output=True,text=True,timeout=180).stdout
        (work/'report-session-wire.txt').write_text(report_result)

        namespace=work/'client-namespace-session';ns_path=mount/'namespace-session-fence';ns_path.mkdir()
        run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),source/'tests/live/client_namespace_session.c',source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',namespace])
        ns_ino=ns_path.stat().st_ino;ns_saved=work/'namespace-session-packets.bin'
        ns_untouched=mount/'namespace-first-touch';ns_untouched.mkdir()
        ns_result=run([namespace,str(a.port),str(ns_ino),source/'efs-mgmt',ns_saved,str(ns_untouched.stat().st_ino)],capture_output=True,text=True,timeout=180).stdout
        (work/'namespace-session-wire.txt').write_text(ns_result)
        retained=work/'client-retained-lease'
        run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),source/'tests/live/client_retained_lease.c',source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',retained])
        edge_results.append(run([retained,str(a.port),str(edge_ino),source/'efs-mgmt'],capture_output=True,text=True,timeout=90).stdout)
        certificate_match=re.search(r'storage-session-certificate uuid=([0-9a-f]{32}) reject_below=(\d+)',edge_results[-1])
        assert certificate_match,edge_results[-1]
        certificate_reader=work/'storage-session-fence'
        run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),source/'tests/live/storage_session_fence.c',source/'src/server/put_fence.c',source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',certificate_reader])
        certificate_args=[str(certificate_reader),certificate_match[1],certificate_match[2],*map(str,roots)]
        def certificate_ready():
            result=subprocess.run(certificate_args,capture_output=True,text=True,env=env)
            if result.returncode==0:(work/'storage-session-certificates.txt').write_text(result.stdout)
            return result.returncode==0
        wait(certificate_ready,'all four members durably fence old PUT guards',40)
        if a.storage_session_fence_fault:
            assert any('test-put-storage-fence: refusing certificate sync' in (work/f'n{i+1}.log').read_text() for i in range(4)), 'certificate fault never exercised'

        tickets=work/'client-put-ticket'
        run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),source/'tests/live/client_put_ticket.c',source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',tickets])
        ticket_results=[]
        for discard in (0,1):
            ticket_results.append(run([tickets,str(a.port),str(edge_ino),source/'efs-mgmt',str(discard)],capture_output=True,text=True,timeout=90).stdout)
        ticket_saved=work/'put-ticket.bin'
        ticket_results.append(run([tickets,str(a.port),str(edge_ino),source/'efs-mgmt','save',ticket_saved],capture_output=True,text=True,timeout=90).stdout)
        (work/'put-ticket-wire.txt').write_text(''.join(ticket_results))


        for stage in ('open','close'):
            edge_results.append(run([client_edge,str(a.port),str(edge_ino),edge_uuid,stage],capture_output=True,text=True,timeout=90).stdout)
        unmount();stop(crash=True);start();mount_client()
        ns_result+=run([namespace,str(a.port),str(ns_ino),source/'efs-mgmt','cold',ns_saved],capture_output=True,text=True,timeout=90).stdout
        wait(certificate_ready,'cold restart preserves four storage session floors',40)
        report_result+=run([report,str(a.port),str(edge_ino),source/'efs-mgmt','cold',report_saved],capture_output=True,text=True,timeout=90).stdout
        (work/'report-session-wire.txt').write_text(report_result)

        (work/'namespace-session-wire.txt').write_text(ns_result)
        (ns_path/'a').unlink();(ns_path/'still-empty').rmdir();ns_path.rmdir();ns_untouched.rmdir()
        edge_results.append(run([client_edge,str(a.port),str(edge_ino),edge_uuid,'replay'],capture_output=True,text=True,timeout=90).stdout)
        ticket_results.append(run([tickets,str(a.port),str(edge_ino),source/'efs-mgmt','cold',ticket_saved],capture_output=True,text=True,timeout=90).stdout)
        (work/'put-ticket-wire.txt').write_text(''.join(ticket_results))
        assert edge_path.read_bytes()==edge_body,'lease transport changed cold bytes'
        edge_results.append(run([client_edge,str(a.port),str(edge_ino),edge_uuid,'cached',source/'efs-mgmt'],capture_output=True,text=True,timeout=90).stdout)
        assert edge_path.read_bytes()==edge_body+bytes(8),'legacy cached append fence corrupts original bytes/hole'
        edge_results.append(run([client_edge,str(a.port),str(edge_ino),edge_uuid,'revoked'],capture_output=True,text=True,timeout=90).stdout)
        edge_path.unlink();wait(lambda:not files(edge_ino),'actual client lease close physical reclaim',180)
        (work/'client-lease-transport.json').write_text(json.dumps(edge_results,indent=2))
        print('actual client sequenced lease OPEN/CLOSE/probe/exact cold replay PASS',flush=True)
        helper=work/'lease-edges'
        run(['cc','-Wall','-Wextra','-I'+str(source/'include'),source/'tests/live/lease_edges.c',source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',helper])
        try:
            namespace_guard=run([helper,'--namespace-fence',str(a.port)],capture_output=True,text=True,timeout=45).stdout
        except subprocess.TimeoutExpired as failure:
            def diagnostic_text(value):
                return value.decode(errors='replace') if isinstance(value,bytes) else value or ''
            (work/'namespace-guard-timeout.txt').write_text(diagnostic_text(failure.stdout)+'\nSTDERR\n'+diagnostic_text(failure.stderr))
            raise
        assert 'two concurrent receipt PREPAREs' in namespace_guard,namespace_guard
        (work/'namespace-guard-wire.txt').write_text(namespace_guard)
        guard_path=mount/'legacy-close-guard'
        # Ordinary unleased CREATE isolates the legacy wire guard from
        # mounted CREATE adoption: CLEAR_PENDING queues its own CLOSE_STEP,
        # even while a normal descriptor remains open. Never overwrite it.
        for node in range(4):
            guard_create=subprocess.run([str(source/'efs-mgmt'),'raft-create',
                f'127.0.0.1:{a.port+node}','1',guard_path.name],env=env,
                capture_output=True,text=True,timeout=30)
            if guard_create.returncode==0 and 'status=0' in guard_create.stdout:break
        else:raise AssertionError('legacy guard CREATE failed: '+guard_create.stdout+guard_create.stderr)
        guard_ino=int(re.search(r'ino=(\d+)',guard_create.stdout).group(1))
        assert guard_path.stat().st_ino==guard_ino
        guarded_wire=run([helper,'--legacy-guard',str(a.port),str(guard_ino)],capture_output=True,text=True,timeout=45).stdout
        assert 'later RESOLVE and retry' in guarded_wire,guarded_wire
        (work/'legacy-close-guard.txt').write_text(guard_create.stdout+guarded_wire)
        guard_path.unlink();wait(lambda:not files(guard_ino),'legacy prepared-close guard physical reclaim',180)
        def legacy_guard_inode_gone():
            for node in range(4):
                result=subprocess.run([str(source/'efs-mgmt'),'raft-getattr',
                    f'127.0.0.1:{a.port+node}',str(guard_ino)],env=env,
                    capture_output=True,text=True,timeout=10)
                if result.returncode==0 and re.search(r'status=1\b',result.stdout):return True
            return False
        wait(legacy_guard_inode_gone,'legacy prepared-close guard inode reclaimed',90)

        # Error constants are extracted from the build header, not host errno.
        header=(source/'include/efs/common.h').read_text()
        def error(name):
            return int(re.search(r'#define\s+'+name+r'\s+(-?\d+)',header).group(1))
        stale=error('EFS_ERR_STALE');missing=error('EFS_ERR_NOT_FOUND');busy=error('EFS_ERR_BUSY')
        path=mount/'guarded-lease';body=b'guarded lease cold payload'*100
        with path.open('wb') as f:f.write(body);f.flush();os.fsync(f.fileno())
        ino=path.stat().st_ino
        # These fresh CREATE rows have initial generation 1 (no truncate/reuse).
        attr=run([helper,str(a.port),str(ino)],capture_output=True,text=True).stdout
        gen=int(re.search(r'gen=(\d+)',attr).group(1))
        uuid=os.urandom(16).hex();records=[]
        def edge(seq,ack,flags,probe=0,expected=0,discard=0):
            result=run([helper,str(a.port),str(ino),str(gen),uuid,str(seq),str(ack),str(flags),str(probe),str(expected),str(discard)],capture_output=True,text=True).stdout
            records.append({'seq':seq,'ack':ack,'flags':flags,'probe':probe,'expected':expected,'discard':discard,'result':result.strip()})
            return result
        shard=int(re.search(r'shard=(\d+)',attr).group(1))
        edge(1,0,1,probe=1,expected=busy)
        for operation,extra in [('create',['1']),('register',[str(shard),'1']),('establish',[str(shard),'1'])]:
            run([source/'efs-mgmt','raft-session',f'127.0.0.1:{a.port}',operation,uuid,*extra])
        owner_uuid=os.urandom(16).hex()
        for operation,extra in [('create',['1']),('register',[str(shard),'1']),('establish',[str(shard),'1'])]:
            run([source/'efs-mgmt','raft-session',f'127.0.0.1:{a.port}',operation,owner_uuid,*extra])
        owner_result=run([helper,'--owner-fence',str(a.port),owner_uuid,str(shard)],capture_output=True,text=True).stdout
        (work/'prepared-owner-wire.txt').write_text(owner_result)
        run([source/'efs-mgmt','raft-session',f'127.0.0.1:{a.port}','fence',owner_uuid])
        edge(1,0,1,probe=1,expected=busy)
        edge(2,0,2);edge(1,0,1,expected=stale)
        # Send to the known leader so the intentionally lost reply is a commit.
        # Try each node; querying does not repeat or create the original effect.
        for node in range(4):
            run([helper,str(a.port+node),str(ino),str(gen),uuid,'3','0','1','0','0','1'])
            try:
                edge(3,0,1,probe=1)
                break
            except subprocess.CalledProcessError:
                if node==3:raise
        unmount();stop(crash=True);start();mount_client()
        assert path.read_bytes()==body
        edge(3,0,1,probe=1)
        path.unlink()
        attr=run([helper,str(a.port),str(ino)],capture_output=True,text=True).stdout
        assert 'nlink=0' in attr,attr
        edge(4,3,2);edge(4,3,2,probe=1)
        unmount();stop(crash=True);start();mount_client()
        edge(4,3,2,probe=1)
        edge(5,4,1,expected=missing);edge(3,0,1,expected=stale)
        wait(lambda:not files(ino),'guarded last-close physical reclaim',180)
        # A read-only unknown query must never acquire ownership.
        path=mount/'guarded-probe-only';path.write_bytes(b'probe only')
        ino=path.stat().st_ino;gen=1
        edge(6,5,1,probe=1,expected=busy)
        path.unlink()
        def reclaimed():
            attr=run([helper,str(a.port),str(ino)],capture_output=True,text=True).stdout
            return 'status=1 ' in attr
        wait(reclaimed,'unknown probe acquires no inode ownership',30)
        # Durable administrative revocation clears a real active edge and
        # rejects delayed old-epoch requests; a disconnect alone does not.
        path=mount/'guarded-fenced';path.write_bytes(b'fenced owner')
        ino=path.stat().st_ino;gen=1
        attr=run([helper,str(a.port),str(ino)],capture_output=True,text=True).stdout
        shard=int(re.search(r'shard=(\d+)',attr).group(1))
        for operation in ('register','establish'):
            session_admission([source/'efs-mgmt','raft-session',f'127.0.0.1:{a.port}',operation,uuid,str(shard),'1'],busy)
        edge(6,5,1);path.unlink()
        run([source/'efs-mgmt','raft-session',f'127.0.0.1:{a.port}','fence',uuid])
        edge(7,6,1,expected=stale)
        wait(reclaimed,'durably fenced active lease inode reclaimed',60)
        wait(lambda:not files(ino),'durably fenced physical fragments reclaimed',180)
        (work/'guarded-leases.json').write_text(json.dumps(records,indent=2))
        print('guarded lease reorder/lost-reply/read-only probe/cold replay/final-close reclaim PASS',flush=True)
    if a.session_append:
        helper=work/'session-append';inspector=work/'session-append-inspect'
        for output,cfile in ((helper,'session_append.c'),(inspector,'close_step.c')):
            run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),source/'tests/live'/cfile,source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',output])
        path=mount/'session-append';body=bytes(range(256))*4096
        with path.open('wb') as out:out.write(body);out.flush();os.fsync(out.fileno())
        ino=path.stat().st_ino;shard=ino & 4095;uuid=os.urandom(16).hex()
        records=[]
        for operation,extra in [('create',['1']),('register',[str(shard),'1']),('establish',[str(shard),'1'])]:
            records.append(run([source/'efs-mgmt','raft-session',f'127.0.0.1:{a.port}',operation,uuid,*extra],capture_output=True,text=True).stdout)
        # Retain a real owner lease: a previous FUSE CLOSE obligation must not
        # race this gate's exact 32-reservation cold ownership snapshot.
        records.append(run([helper,'hold',str(a.port),uuid,str(ino)],capture_output=True,text=True).stdout)
        reserved=run([inspector,'reserve',str(a.port),str(ino),uuid,'64','131072'],capture_output=True,text=True,timeout=180).stdout
        records.append(run([helper,'fence-half',str(a.port),uuid,str(ino)],capture_output=True,text=True,timeout=180).stdout)
        records.append(run([inspector,'late',str(a.port),str(ino),uuid,'131072'],capture_output=True,text=True,timeout=45).stdout)
        unmount();stop(crash=True)
        before=[run([inspector,'inspect',root/'mdraft/kv',str(ino)],capture_output=True,text=True).stdout for root in roots]
        assert any('open=32 ' in text for text in before),before
        start(prefer_single_host=True);mount_client()
        routing=run([source/'efs-mgmt','raft-status',f'127.0.0.1:{a.port}'],capture_output=True,text=True).stdout
        assert re.search(r'group 0 hosted=1 role=LEADER',routing) and re.search(r'group 2 hosted=0',routing),routing
        for replay in range(2):
            result=run([source/'efs-mgmt','raft-session',f'127.0.0.1:{a.port}','cleanup-appends',uuid,'1',str(shard)],capture_output=True,text=True,timeout=180)
            assert 'rc=0' in result.stdout,result.stdout
            records.append(result.stdout)
        records.append(run([inspector,'late',str(a.port),str(ino),uuid,'131072'],capture_output=True,text=True,timeout=45).stdout)
        expected=len(body)+64*131072
        assert path.stat().st_size==expected and path.read_bytes()==body+bytes(expected-len(body)), 'revoked append cleanup corrupts original bytes/holes'
        unmount();stop()
        after=[run([inspector,'inspect',root/'mdraft/kv',str(ino)],capture_output=True,text=True).stdout for root in roots]
        assert all('open=0 ' in text and 'reservations=0 ' in text and 'receipts=0' in text for text in after),after
        start();mount_client()
        assert path.stat().st_size==expected and path.read_bytes()==body+bytes(expected-len(body))
        released=run([source/'efs-mgmt','raft-session',f'127.0.0.1:{a.port}','cleanup-leases',uuid,'1',str(shard)],capture_output=True,text=True,timeout=180)
        assert 'rc=0' in released.stdout,released.stdout
        records.append(released.stdout)
        path.unlink();wait(lambda:not files(ino),'revoked append physical reclaim',180)
        (work/'session-append.json').write_text(json.dumps({'ino':ino,'reserved':reserved,'records':records,'before':before,'after':after,'routing':routing,'expected_bytes':expected},indent=2))
        print('bounded revoked-session append: 32+32 cold transactions, exact retry, original bytes/holes, physical reclaim PASS',flush=True)
    if a.deferred_close:
        helper=work/'deferred-lease';inspector=work/'close-step'
        for output,cfile in ((helper,'lease_edges.c'),(inspector,'close_step.c')):
            run(['cc','-Wall','-Wextra','-I'+str(source/'include'),source/'tests/live'/cfile,source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',output])
        path=mount/'deferred-close';body=bytes(range(256))*4096
        with path.open('wb') as out:out.write(body);out.flush();os.fsync(out.fileno())
        ino=path.stat().st_ino;gen=1;uuid=os.urandom(16).hex()
        attr=run([helper,str(a.port),str(ino)],capture_output=True,text=True).stdout
        shard=int(re.search(r'shard=(\d+)',attr).group(1))
        for operation,extra in [('create',['1']),('register',[str(shard),'1']),('establish',[str(shard),'1'])]:
            run([source/'efs-mgmt','raft-session',f'127.0.0.1:{a.port}',operation,uuid,*extra])
        deferred_env=dict(env,EFS_TEST_DEFERRED_LEASE='1');records=[]
        def deferred(seq,ack,flags,probe=0,expected=0):
            result=subprocess.run([str(helper),str(a.port),str(ino),str(gen),uuid,str(seq),str(ack),str(flags),str(probe),str(expected),'0'],env=deferred_env,capture_output=True,text=True,check=True,timeout=45).stdout
            records.append(result.strip());return result
        deferred(1,0,1)
        reserved=run([inspector,'reserve',str(a.port),str(ino),uuid,'64','131072'],capture_output=True,text=True,timeout=180).stdout
        expected=len(body)+64*131072
        assert f'first={len(body)} end={expected}' in reserved,reserved
        deferred(2,1,2);deferred(2,1,2,probe=1)
        unmount();stop(crash=True)
        # Opening these LSMs is only permitted while ALL private daemons stop.
        before=[run([inspector,'inspect',root/'mdraft/kv',str(ino)],capture_output=True,text=True).stdout for root in roots]
        assert any(int(re.search(r'open=(\d+)',text).group(1))>0 for text in before),before
        assert any(int(re.search(r'markers=(\d+)',text).group(1))>0 for text in before),before
        start(prefer_single_host=True);mount_client()
        routing=run([source/'efs-mgmt','raft-status',f'127.0.0.1:{a.port}'],capture_output=True,text=True).stdout
        assert re.search(r'group 0 hosted=1 role=LEADER',routing),routing
        assert re.search(r'group 2 hosted=0',routing),routing
        def settled():
            try:return path.stat().st_size==expected
            except OSError:return False
        wait(settled,'cold deferred close drains OPEN append frontier',180)
        assert path.read_bytes()==body+bytes(expected-len(body)),'deferred close corrupts original bytes or holes'
        # Give exact DONE-page cleanup time to retire its obligation. The
        # offline snapshot remains the final oracle, never pathname absence.
        time.sleep(12)
        unmount();stop()
        after=[run([inspector,'inspect',root/'mdraft/kv',str(ino)],capture_output=True,text=True).stdout for root in roots]
        for text in after:
            assert 'markers=0 reservations=0' in text and 'open=0 ' in text and 'locks=0' in text,text
        start();mount_client();deferred(2,1,2,probe=1)
        deferred(3,2,1);assert path.read_bytes()==body+bytes(expected-len(body))
        deferred(4,3,2)
        stale=int(re.search(r'#define\s+EFS_ERR_STALE\s+(-?\d+)',(source/'include/efs/common.h').read_text()).group(1))
        deferred(2,1,2,expected=stale)
        path.unlink();wait(lambda:not files(ino),'deferred-close physical reclaim',180)
        (work/'deferred-close.json').write_text(json.dumps({'ino':ino,'reserved':reserved.strip(),'before':before,'after':after,'edges':records,'expected_bytes':expected,'single_host_leader':routing},indent=2))
        print('identified deferred close, cold 64-append holes, exact retirement and reopened bytes PASS',flush=True)
    if a.create_lease:
        helper=work/'create-lease'
        run(['cc','-I'+str(source/'include'),source/'tests/live/create_lease.c',source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',helper])
        cases=[]
        for number,hashed in enumerate((False,True)):
            folder=mount/('lease-hashed' if hashed else 'lease-local');folder.mkdir()
            parent=folder.stat().st_ino
            if hashed:
                for operation in ('begin','finish'):
                    for node in range(4):
                        result=subprocess.run([str(source/'efs-mgmt'),'raft-dir',f'127.0.0.1:{a.port+node}',str(parent),operation],capture_output=True,text=True,timeout=30,env=env)
                        if result.returncode==0 and 'status=0' in result.stdout:break
                    else:raise AssertionError('CREATE lease spread failed: '+result.stdout+result.stderr)
            owner=93000000+os.getpid()*2+number
            output=run([helper,str(a.port),str(parent),str(owner),'0'],capture_output=True,text=True).stdout.split()
            assert len(output)==3,output
            cases.append({'parent':parent,'owner':owner,'ino':int(output[0]),'name':output[1],'hashed_shard':int(output[2]),'hashed':hashed})
        time.sleep(1)
        unmount();stop(crash=True);start();mount_client()
        for case in cases:
            run([helper,str(a.port),str(case['parent']),str(case['owner']),str(case['ino'])])
        # The helper used inode RPCs; remount before checking cached directories.
        unmount();mount_client()
        for folder in ('lease-local','lease-hashed'):(mount/folder).rmdir()
        (work/'create-lease.json').write_text(json.dumps(cases,indent=2))
        print('atomic CREATE lease local/first-use cross-group cold replay and final close PASS',flush=True)
    if a.append_sync_boundary or a.sync_writes:
        peer_log=open(work/'sync-peer.log','ab');handles.append(peer_log)
        peer=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{a.port}','default',str(peer_mount),'-f'],env=env,stdout=peer_log,stderr=peer_log)
        wait(lambda:os.path.ismount(peer_mount) and (peer_mount.stat().st_ino>0),'sync peer mounted',40)
        transport_proof([*servers,client,peer])
        records=[];expected={}
        writer_code="""import os,sys,time,json
path,flags,size,explicit=sys.argv[1],int(sys.argv[2]),int(sys.argv[3]),int(sys.argv[4])
fd=os.open(path,flags);body=b'w'*size;start=time.monotonic()
if explicit:
 done=0
 while done<size:
  n=os.write(fd,body[done:done+(128<<10)]);assert n>0;done+=n
  os.fsync(fd)
else:
 assert os.write(fd,body)==size,'synchronous write short-returned'
print(json.dumps({'seconds':time.monotonic()-start,'bytes':size}),flush=True)
assert sys.stdin.readline().strip()=='release'
os.close(fd)
"""
        modes=([('fsync',0)] if a.append_sync_boundary else [])+(([('mount-sync',0)] if a.mount_sync else [('sync',os.O_SYNC),('dsync',os.O_DSYNC)]) if a.sync_writes else [])
        for label,flag in modes:
            for append in ((False,True) if a.sync_writes else (True,)):
                for size in (31,2<<20):
                    name=f'synchronous-{label}-{append}-{size}'
                    path=mount/name;prefix=b'prefix\n' if append else b''
                    with path.open('xb',buffering=0) as out:
                        if prefix:assert out.write(prefix)==len(prefix)
                        os.fsync(out.fileno())
                    flags=os.O_WRONLY|flag|(os.O_APPEND if append else 0)
                    writer=subprocess.Popen(['python3','-c',writer_code,str(path),str(flags),str(size),str(int(label=='fsync'))],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env)
                    try:
                        assert select.select([writer.stdout],[],[],40)[0],'synchronous write exceeded budget'
                        line=writer.stdout.readline();assert line,writer.stderr.read()
                        record=json.loads(line)
                        body=prefix+b'w'*size
                        assert (peer_mount/name).read_bytes()==body,'synchronous bytes not published before close'
                        record.update(name=name,append=append,flag=label,verified_before_close=True)
                        records.append(record);expected[name]=hashlib.sha256(body).hexdigest()
                        writer.stdin.write('release\n');writer.stdin.flush()
                        assert writer.wait(timeout=10)==0,writer.stderr.read()
                    finally:
                        if writer.poll() is None:writer.kill();writer.wait()
        # A different client extends another lane between append bursts.
        # The next reservation must use that authoritative EOF, including
        # holes, rather than the reserving host's incomplete local lanes.
        for gap_chunks in (5,6):
            name=f'append-peer-extension-{gap_chunks}';path=mount/name
            first=b'A'*(128<<10);extension=b'E'*(128<<10);tail=b'T'*(128<<10)
            fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_WRONLY|os.O_APPEND,0o600)
            try:
                assert os.write(fd,first)==len(first);os.fsync(fd)
                pfd=os.open(peer_mount/name,os.O_WRONLY)
                try:
                    assert os.pwrite(pfd,extension,gap_chunks*(128<<10))==len(extension)
                    os.fsync(pfd)
                finally:os.close(pfd)
                assert os.write(fd,tail)==len(tail);os.fsync(fd)
            finally:os.close(fd)
            body=first+bytes((gap_chunks-1)*(128<<10))+extension+tail
            assert (peer_mount/name).read_bytes()==body,'append ignored peer lane EOF'
            expected[name]=hashlib.sha256(body).hexdigest()
            records.append({'name':name,'bytes':len(body),'mixed_peer_extension':True,
                            'gap_chunks':gap_chunks,'verified_before_close':False})
        run([source/'scripts/client.sh','stop',peer_mount]);peer.wait(timeout=15);peer=None
        unmount();stop(crash=True);start();mount_client()
        for name,digest in expected.items():
            assert hashlib.sha256((mount/name).read_bytes()).hexdigest()==digest,'synchronous bytes changed after server crash'
            (mount/name).unlink()
        if a.sync_writes and not a.mount_sync:
            trace=(work/'fuse.log').read_text(errors='replace')
            assert len(re.findall(r'sync-write ino=.* rc=0',trace))>=8,'synchronous callback publication trace missing'
        result={'cases':records,'cold_crash_verified':True,'explicit_fsync_before_close':a.append_sync_boundary,
                'synchronous_write_before_close':a.sync_writes,'mount_sync':a.mount_sync}
        (work/('synchronous-writes.json' if a.sync_writes else 'append-sync-boundary.json')).write_text(json.dumps(result,indent=2))
        wait(drained,'synchronous write physical deletion drained')
        print(('Mount-wide sync ordinary small/queued/append before close and cold crash bytes' if a.mount_sync else 'O_SYNC/O_DSYNC small/queued/append before close and cold crash bytes' if a.sync_writes else 'Append/fsync across lane groups: peer before close and cold crash bytes')+' PASS',flush=True)
    if a.mount_sync:
        print('Mount-wide sync acceptance complete; physical deletion drained PASS',flush=True)
        raise SystemExit(0)
    if a.append_replay_race:
        helper=work/'append-replay-race'
        run(['cc','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-I'+str(source/'include'),
             source/'tests/live/append_replay_race.c','-pthread','-o',helper])
        path=mount/'append-identical-rpc'
        fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
        ino_replay=os.fstat(fd).st_ino
        def replay(label,sequence=1,ack=0,length=8,verdict=0,offset=0):
            result=subprocess.run([str(helper),str(a.port),str(ino_replay),
                                   str(sequence),str(ack),str(length),str(verdict),str(offset)],
                                  capture_output=True,text=True,timeout=120,env=env)
            (work/('append-replay-'+label+'.log')).write_text(result.stdout+result.stderr)
            assert result.returncode==0,result.stdout+result.stderr
        try:
            replay('initial')
            assert os.pwrite(fd,b'append-A',0)==8;os.fsync(fd)
        finally:os.close(fd)
        unmount();stop();start();mount_client()
        replay('cold-retry')
        group=0 if ino_replay & 1 else 2
        leaders=[]
        for node in range(4):
            text=run([source/'efs-mgmt','raft-status',f'127.0.0.1:{a.port+node}'],capture_output=True,text=True).stdout
            if re.search(r'group '+str(group)+r' .*role=LEADER',text):leaders.append(node)
        assert len(leaders)==1,leaders
        dead=leaders[0];servers[dead].kill();servers[dead].wait(timeout=10)
        replay('leader-death')
        (work/'append-leader-death.json').write_text(json.dumps({'group':group,'killed_node':dead+1,'same_identity_replayed':True,'offset':0},indent=2))
        unmount();stop();start();mount_client()
        assert path.read_bytes()==b'append-A','reserve replay changed the cold file extent'
        # ACK frees the original receipt; old requests must fail closed.
        replay('acknowledged-next',2,1,8,0,8)
        replay('retired',1,0,8,11,0) # EFS_INODE_RPC_STALE
        replay('changed-payload',2,1,9,6,0) # EFS_INODE_RPC_INVAL
        fd=os.open(path,os.O_WRONLY)
        try:assert os.pwrite(fd,b'append-B',8)==8;os.fsync(fd)
        finally:os.close(fd)
        unmount();stop();start();mount_client()
        replay('cold-next',2,1,8,0,8)
        replay('cold-retired',1,0,8,11,0)
        assert path.read_bytes()==b'append-Aappend-B','durable replay duplicated or changed bytes'
        path.unlink();wait(drained,'identical append RPC physical deletion drained')
        print('durable append lost replies, 32 identical RPCs, ACK retirement and exact cold bytes PASS',flush=True)
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
    if a.transaction_apply_retry:
        assert a.server_binary,'transaction retry gate requires an explicitly selected fault build'
        if a.transaction_fault_seed:
            # Restart only our private seed after peers are already available,
            # so it rejoins as a follower rather than remaining group leader.
            old=servers[0];old.send_signal(signal.SIGTERM);old.wait(timeout=20)
            time.sleep(3)
            log=open(work/'n1.log','ab');handles.append(log)
            seed_env=dict(env,EFS_FAULT_TXN_APPLY_FILE=str(work/'transaction-apply-fault-1'))
            servers[0]=subprocess.Popen(old.args,env=seed_env,stdout=log,stderr=log)
            time.sleep(3)
        target=None
        for candidate in ((0,1,2,3) if a.transaction_fault_seed else (2,1,3,0)):
            text=run([source/'efs-mgmt','raft-status',f'127.0.0.1:{a.port+candidate}'],capture_output=True,text=True).stdout
            roles=re.findall(r'role=([-\w]+)',text)
            pairs=re.findall(r'commit=(\d+) applied=(\d+)',text)
            if len(roles)==2 and len(pairs)==2 and 'LEADER' not in roles and any(int(commit)>0 for commit,applied in pairs):
                target=candidate;break
        assert target is not None,'no actual caught-up follower to fault'
        if a.transaction_fault_seed:assert target==0,'seed did not rejoin as a replicated follower'
        def transaction_lag():
            text=run([source/'efs-mgmt','raft-status',f'127.0.0.1:{a.port+target}'],capture_output=True,text=True).stdout
            roles=re.findall(r'role=([-\w]+)',text)
            pairs=re.findall(r'commit=(\d+) applied=(\d+)',text)
            assert len(roles)==2 and len(pairs)==2 and 'LEADER' not in roles,text
            return max(int(commit)-int(applied) for commit,applied in pairs)
        path=mount/'txn-retry-source';body=b'transaction durable retry payload'*1024
        with path.open('xb') as out:out.write(body);out.flush();os.fsync(out.fileno())
        wait(lambda:transaction_lag()==0,'transaction follower baseline caught up',60)
        fault=work/f'transaction-apply-fault-{target+1}';fault.touch()
        linked=mount/'txn-retry-linked';moved=mount/'txn-retry-moved'
        os.link(path,linked);linked.rename(moved)
        wait(lambda:transaction_lag()>0,'transaction persistence failure retains follower apply lag',60)
        assert 'txn-fault: refused persistence' in (work/f'n{target+1}.log').read_text(),'fault hook did not execute'
        first=transaction_lag();time.sleep(2);second=transaction_lag()
        assert first>0 and second>0,'failed follower advanced its pending transaction'
        fault.unlink();wait(lambda:transaction_lag()==0,'transaction exact apply retries catch up after fault release',90)
        assert path.read_bytes()==body and moved.read_bytes()==body
        unmount();stop(crash=True);start();mount_client()
        assert path.read_bytes()==body and moved.read_bytes()==body
        assert path.stat().st_ino==moved.stat().st_ino and path.stat().st_nlink==2
        path.unlink();moved.unlink()
        (work/'transaction-apply-retry.json').write_text(json.dumps({'follower_node':target+1,'held_lag':[first,second],'cold_exact_bytes':True,'cold_nlink':2},indent=2))
        print('transaction follower persistence refusal, held apply index, exact retry and cold linked namespace PASS',flush=True)
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
    if a.sync_write_fault:
        records=[];expected={}
        for label,flag in [('sync',os.O_SYNC),('dsync',os.O_DSYNC)]:
            name='synchronous-offline-'+label;target=mount/name
            body=b'synchronous callback retains this partial body'
            fd=os.open(target,os.O_CREAT|os.O_EXCL|os.O_WRONLY|flag,0o600)
            stop();errors=[]
            trace_start=(work/'fuse.log').stat().st_size
            try:
                try:os.write(fd,body)
                except OSError as error:
                    assert error.errno==5,error;errors.append({'call':'write','errno':error.errno})
                else:raise AssertionError('offline synchronous write falsely succeeded')
                with (work/'fuse.log').open('rb') as trace_file:
                    trace_file.seek(trace_start);trace=trace_file.read().decode(errors='replace')
                assert re.search(r'sync-write ino=.* bytes='+str(len(body))+r' .* rc=-5',trace),'write did not reach the synchronous publication failure boundary'
                start()
                for attempt in range(3):
                    try:os.fsync(fd);break
                    except OSError as error:
                        assert error.errno==5 and attempt<2,error
                        errors.append({'call':'recovery-fsync','errno':error.errno})
            finally:os.close(fd)
            expected[name]=hashlib.sha256(body).hexdigest()
            records.append({'flag':label,'bytes':len(body),'errors':errors,'recovery_fsync_succeeded':True})
        unmount();stop(crash=True);start();mount_client()
        for name,digest in expected.items():
            assert hashlib.sha256((mount/name).read_bytes()).hexdigest()==digest,'synchronous failed-write recovery lost bytes'
            (mount/name).unlink()
        (work/'synchronous-write-fault.json').write_text(json.dumps({'cases':records,'cold_crash_verified':True,'actual_callback_failure_traced':True},indent=2))
        wait(drained,'synchronous failed-write recovery physically reclaimed')
        print('O_SYNC/O_DSYNC offline callback failure retains exact bytes for fsync and cold recovery PASS',flush=True)
    if a.stop_dirty_recovery:
        target=mount/'stop-new-partial';body=b'new partial body retained through PUT failure'
        fd=os.open(target,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600)
        assert os.write(fd,body)==len(body)
        stop()
        failures=[]
        try:
            try:os.fsync(fd)
            except OSError as error:failures.append({'call':'fsync','errno':error.errno})
            else:raise AssertionError('offline fsync falsely acknowledged a new partial body')
        finally:
            try:os.close(fd)
            except OSError as error:failures.append({'call':'close','errno':error.errno})
        start()
        # Dirty-stop acceptance already permits 30 seconds. The generic 15-second
        # child wait must not preempt that contract while retained bytes drain.
        started=time.monotonic();unmount(timeout=30);elapsed=time.monotonic()-started
        assert elapsed<30,'recovered new partial body did not cleanly drain'
        stop();start();mount_client()
        assert target.read_bytes()==body,'clean stop lost the retained partial body'
        (work/'stop-dirty-recovery.json').write_text(json.dumps({
            'bytes':len(body),'failures':failures,'clean_stop_seconds':elapsed,
            'cold_sha256':hashlib.sha256(body).hexdigest(),'forced_discard':False},indent=2))
        target.unlink();wait(drained,'recovered partial body physically reclaimed')
        print('failed new partial flush: clean stop drains retained body and exact cold bytes PASS',flush=True)
    if a.shutdown_mutations:
        directory=mount/'shutdown-writes';directory.mkdir()
        acknowledged=[];errors=[];active=threading.Event();finish=threading.Event()
        def writer():
            try:
                for index in range(4096):
                    if finish.is_set():break
                    body=index.to_bytes(8,'little')+bytes(range(256))*128
                    active.set()
                    with (directory/str(index)).open('xb',buffering=0) as out:
                        assert out.write(body)==len(body);os.fsync(out.fileno())
                    acknowledged.append((index,hashlib.sha256(body).hexdigest()))
            except OSError as error:errors.append(str(error))
        thread=threading.Thread(target=writer,daemon=True);thread.start()
        wait(lambda:len(acknowledged)>=8,'fsynced mutations before leader stop',30)
        assert active.wait(1) and thread.is_alive()
        leaders=[]
        for node in range(4):
            text=run([source/'efs-mgmt','raft-status',f'127.0.0.1:{a.port+node}'],capture_output=True,text=True).stdout
            if re.search(r'group 0 .*role=LEADER',text):leaders.append(node)
        assert len(leaders)==1,leaders
        node=leaders[0];proc=servers[node]
        command=Path(f'/proc/{proc.pid}/cmdline').read_bytes().split(b'\0')[:-1]
        assert thread.is_alive(),'writer finished before the shutdown signal'
        stalled=[];churn_stop=threading.Event()
        def connection_churn():
            while not churn_stop.is_set():
                try:
                    with socket.create_connection(('127.0.0.1',a.port+node),timeout=.1):pass
                except OSError:pass
                churn_stop.wait(.005)
        churn=threading.Thread(target=connection_churn,daemon=True)
        try:
            for index in range(32):
                sock=socket.create_connection(('127.0.0.1',a.port+node),timeout=1)
                stalled.append(sock)
                if index%2:sock.sendall(b'\0\0')
            churn.start()
            assert thread.is_alive(),'writer finished before reader/churn shutdown'
            before_stop=len(acknowledged)
            started=time.monotonic();proc.terminate();proc.wait(timeout=10)
            elapsed=time.monotonic()-started;assert proc.returncode==0,proc.returncode
        finally:
            churn_stop.set()
            if churn.is_alive():churn.join(2)
            assert not churn.is_alive(),'connection churn did not retire'
            for sock in stalled:sock.close()
        finish.set();thread.join(60);assert not thread.is_alive(),'mutation worker did not finish'
        log=open(work/f'n{node+1}.log','ab');handles.append(log)
        servers[node]=subprocess.Popen(command,env=env,stdout=log,stderr=log)
        def restarted():
            if servers[node].poll() is not None:raise AssertionError('restarted leader exited')
            try:
                with socket.create_connection(('127.0.0.1',a.port+node),timeout=.2):return True
            except OSError:return False
        wait(restarted,'leader restart responds',30)
        unmount();stop();start();mount_client()
        for index,digest in acknowledged:
            assert hashlib.sha256((directory/str(index)).read_bytes()).hexdigest()==digest,(node,index)
        record={'leader_node':node+1,'shutdown_seconds':elapsed,'acknowledged_before_stop':before_stop,'acknowledged_files':len(acknowledged),'writer_active_at_signal':True,'errors':errors,'cold_bytes_verified':True,'returncode':proc.returncode,'idle_tcp_readers':16,'partial_tcp_frames':16,'connection_churn_at_signal':True,'data_transport':a.transport}
        (work/'shutdown-mutations.json').write_text(json.dumps(record,indent=2))
        for path in directory.iterdir():path.unlink()
        directory.rmdir();wait(drained,'shutdown mutation deletion drains',180)
        print('active mutation graceful leader stop and cold acknowledged-byte recovery PASS',flush=True)
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
        for directory, _, names in os.walk(mount/'mixed-read-gate', topdown=False):
            for name in names:(Path(directory)/name).unlink()
            Path(directory).rmdir()
        wait(drained,'bulk mixed-read deletions drained before fault fixture',600)
        budgets=re.findall(r'buf-budget hard=(\d+) drain=(\d+) slab=(\d+)',(work/'fuse.log').read_text())
        assert budgets and all(tuple(map(int,b))==(hard_budget,drain_budget,32<<20) for b in budgets),budgets
    if a.gc_reap_files:
        # Startup recovery marks all shards; exclude that one-time sweep
        # from the steady-state reaping comparison on these four-node fixtures.
        print('waiting 120 seconds before steady-state reaping capture',flush=True)
        time.sleep(120)
        folder=mount/'reap-latency';folder.mkdir()
        for index in range(a.gc_reap_files):
            with (folder/str(index)).open('xb',buffering=0) as out:
                assert out.write(b'gc-reap-witness')==15;os.fsync(out.fileno())
        wait(drained,'pre-reap queues drained')
        offsets=[(work/f'n{i+1}.log').stat().st_size for i in range(4)]
        before=totals(status());started=time.monotonic()
        for path in folder.iterdir():path.unlink()
        folder.rmdir();wait(drained,'small-file reap queues drained',600)
        elapsed=time.monotonic()-started;after=totals(status());samples=[]
        for repeat in range(3):
            result=run(['python3',source/'tests/measure/md_latency.py',mount],capture_output=True,text=True,timeout=180)
            (work/f'reap-mdlat-{repeat}.log').write_text(result.stdout+result.stderr);samples.append(result.stdout)
        passes=[]
        for node,offset in enumerate(offsets):
            with (work/f'n{node+1}.log').open('rb') as log:log.seek(offset);text=log.read().decode(errors='replace')
            passes.extend({'node':node+1,**{k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)}} for line in text.splitlines() if 'gc-pass ' in line)
        assert after['removed_fragments']>before['removed_fragments'] and after['reclaimed_payload_bytes']>before['reclaimed_payload_bytes']
        (work/'reap-latency.json').write_text(json.dumps({'files':a.gc_reap_files,'deletion_seconds':elapsed,'before':before,'after':after,'metadata_samples':samples,'passes':passes},indent=2))
        wait(drained,'metadata probe cleanup drained',180)
        print('small-file physical reaping and metadata latency capture PASS',flush=True)
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
            # Queue counters describe the last sampled GC pass and may
            # still be zero immediately after unlink. Require actual
            # physical absence as well; scan only when queues appear empty.
            wait(lambda:drained() and not files(ino),
                 'IOR GC records and physical fragments drained',1800)
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
                # ecrawl must receive server allocation on a cold client,
                # without help from the earlier byte-verification reads.
                unmount();mount_client()
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
            if a.namespace_boundary_fault:
                outcomes=[]
                for stage in ('prepare','commit'):
                    replacement=mount/f'fault-replace-{stage}';oldino=spread(replacement)
                    incoming=mount/f'fault-incoming-{stage}';incoming.mkdir();saved=incoming.stat().st_ino
                    barrier=work/'txn-barrier';ready=work/'txn-barrier.ready';barrier.write_text(stage)
                    errors=[]
                    def replace():
                        try:os.rename(incoming,replacement)
                        except OSError as error:errors.append(str(error))
                    thread=threading.Thread(target=replace,daemon=True);thread.start()
                    wait(ready.exists,'64-lane '+stage+' barrier reached',30)
                    proof=ready.read_text();pid=int(re.search(r'pid=(\d+)',proof).group(1))
                    (work/f'namespace-{stage}-barrier.json').write_text(json.dumps({'proof':proof,'incoming_inode':saved,'replacement_inode':oldino},indent=2))
                    node=next(i for i,proc in enumerate(servers) if proc.pid==pid)
                    proc=servers[node];command=Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')[:-1]
                    proc.kill();proc.wait();barrier.unlink();ready.unlink()
                    log=open(work/f'n{node+1}.log','ab');handles.append(log)
                    servers[node]=subprocess.Popen(command,env=env,stdout=log,stderr=log)
                    thread.join(60);assert not thread.is_alive(),'replacement retry did not finish'
                    unmount();stop();start();mount_client()
                    transient=[];deadline=time.monotonic()+30
                    while True:
                        try:observed=replacement.stat().st_ino;break
                        except OSError as error:
                            # Recovery may temporarily lack authoritative
                            # reads; it must never disguise that as absence.
                            assert error.errno in (5,16),error
                            transient.append(error.errno)
                            assert time.monotonic()<deadline,'replacement authority did not recover'
                            time.sleep(.1)
                    if observed==saved:assert not incoming.exists()
                    else:assert observed==oldino and incoming.stat().st_ino==saved
                    if stage=='commit':assert observed==saved,'durable replacement COMMIT was lost'
                    outcomes.append({'stage':stage,'barrier':proof,'coordinator_node':node+1,'errors':errors,'replacement_inode':observed,'incoming_inode':saved,'old_inode':oldino,'transient_read_errno':transient,'cold_atomic_namespace':True})
                    (work/'namespace-boundary-fault.json').write_text(json.dumps(outcomes,indent=2))
                    if incoming.exists():incoming.rmdir()
                    replacement.rmdir();wait(drained,'fault replacement cleanup drained',180)
                (work/'namespace-boundary-fault.json').write_text(json.dumps(outcomes,indent=2))
                print('64-lane prepared/committed coordinator death and cold atomic recovery PASS',flush=True)

    if a.namespace_max:
        # Exercise the actual guarded transaction envelope, including >64 hops.
        attrs_helper=work/'namespace-attrs'
        run(['cc','-Wall','-Wextra','-Werror','-I'+str(source/'include'),source/'tests/live/namespace_attrs.c',source/'libefs.a','-pthread','-lm','-ldl','-libverbs','-o',attrs_helper])
        def authoritative_attrs(path):
            ino=path.stat().st_ino
            return tuple(map(int,run([attrs_helper,str(a.port),str(ino)],capture_output=True,text=True).stdout.split()))
        folder=mount;chain=[];moving=mount/'max-moving';moving.mkdir()
        last=moving;elapsed=None
        for index in range(144):
            folder=folder/f'max-{index}';folder.mkdir();chain.append(folder)
            unchanged=lambda path: tuple(getattr(path.stat(),field) for field in
                ('st_ino','st_mode','st_uid','st_gid','st_nlink','st_mtime_ns','st_ctime_ns'))
            before_source=unchanged(last);before_destination=unchanged(folder)
            if index==143:
                authoritative_source=authoritative_attrs(last)
                authoritative_destination=authoritative_attrs(folder)
            started=time.monotonic()
            try:os.rename(last,folder/'moved')
            except OSError as error:
                assert error.errno==errno.EOVERFLOW,error
                elapsed=time.monotonic()-started
                assert index==143
                assert authoritative_attrs(last)==authoritative_source
                assert authoritative_attrs(folder)==authoritative_destination
                break
            last=folder/'moved'
        assert elapsed is not None and len(chain)==144
        saved=last.stat().st_ino
        assert elapsed<2 and not (folder/'moved').exists()
        assert 'namespace-capacity' in ''.join((work/f'n{i+1}.log').read_text(errors='replace') for i in range(4))
        unmount();stop();start();mount_client()
        assert last.stat().st_ino==saved
        assert authoritative_attrs(last)==authoritative_source
        assert authoritative_attrs(folder)==authoritative_destination
        (work/'namespace-refusal-attrs.json').write_text(json.dumps({
            'authoritative_source':authoritative_source,'authoritative_destination':authoritative_destination,
            'cached_source_before':before_source,'cold_source_after':unchanged(last),
            'cached_destination_before':before_destination,'cold_destination_after':unchanged(folder),
            'authoritative_unchanged_before_and_after_restart':True},indent=2))
        recovered=mount/'max-recovered';os.rename(last,recovered)
        assert recovered.stat().st_ino==saved
        if a.namespace_max_fault:
            outcomes=[]
            destination=chain[-2]/'moved'
            for stage in ('prepare','commit'):
                barrier=work/'txn-barrier';ready=work/'txn-barrier.ready'
                barrier.write_text(stage);errors=[]
                def move_max():
                    try:os.rename(recovered,destination)
                    except OSError as error:errors.append(str(error))
                thread=threading.Thread(target=move_max,daemon=True);thread.start()
                wait(ready.exists,'maximum namespace '+stage+' barrier reached',30)
                proof=ready.read_text()
                (work/f'namespace-max-{stage}-barrier.txt').write_text(proof)
                assert int(re.search(r'parts=(\d+)',proof).group(1))==144,proof
                pid=int(re.search(r'pid=(\d+)',proof).group(1))
                node=next(i for i,proc in enumerate(servers) if proc.pid==pid)
                proc=servers[node];command=Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')[:-1]
                proc.kill();proc.wait();barrier.unlink();ready.unlink()
                log=open(work/f'n{node+1}.log','ab');handles.append(log)
                servers[node]=subprocess.Popen(command,env=env,stdout=log,stderr=log)
                thread.join(60);assert not thread.is_alive(),'maximum namespace retry did not finish'
                unmount();stop();start();mount_client()
                def authoritative_names():
                    try:
                        observed=[path for path in (recovered,destination) if path.exists()]
                        assert len(observed)==1,observed
                        assert observed[0].stat().st_ino==saved
                        if stage=='commit':assert observed[0]==destination,'durable COMMIT lost'
                        return observed[0]
                    except OSError as error:
                        assert error.errno in (5,16),error
                        return None
                observed=None;deadline=time.monotonic()+30
                while observed is None and time.monotonic()<deadline:
                    observed=authoritative_names()
                    if observed is None:time.sleep(.1)
                assert observed is not None,'maximum namespace authority did not recover'
                outcomes.append({'stage':stage,'proof':proof,'coordinator':node+1,
                                 'syscall_errors':errors,'cold_inode':saved,
                                 'destination_committed':observed==destination})
                (work/'namespace-max-fault.json').write_text(json.dumps(outcomes,indent=2))
                if observed==destination:os.rename(destination,recovered)
            print('exact 144-participant prepared/committed coordinator death and cold atomic recovery PASS',flush=True)
        os.rmdir(recovered)
        for directory in reversed(chain):os.rmdir(directory)
        (work/'namespace-max.json').write_text(json.dumps({'accepted_depth':len(chain)-1,'refused_depth':len(chain),'refusal_seconds':elapsed,'refusal_errno':errno.EOVERFLOW,'refused_metadata_unchanged':True,'restart_verified':True},indent=2))
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
    # Mount registration can precede the first verbs connection. Establish
    # actual peer I/O before checking that RDMA was used, never accept TCP.
    assert os.pread(f2,16,0)==bytes(16)
    transport_proof([peer])
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
        try:
            run(['python3',source/'tests/posix/posix_suite.py',mount,'--jobs',str(a.posix_jobs),'--timeout-s','30','--results',work/'posix.tsv'],timeout=600)
        except subprocess.CalledProcessError as error:
            if not a.continue_posix_failure:raise
            posix_failure=error
            (work/'posix-failure.json').write_text(json.dumps({'returncode':error.returncode,'command':list(map(str,error.cmd))},indent=2))
            print('POSIX FAILED; continuing independent gates, final status remains failure',flush=True)
        peer_log=open(work/'peer-fuse.log','ab');handles.append(peer_log)
        peer=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{a.port}','default',str(peer_mount),'-f'],env=env,stdout=peer_log,stderr=peer_log)
        wait(lambda:subprocess.run(['mountpoint','-q',str(peer_mount)]).returncode==0,'peer mounted',40)
        transport_proof([peer])
        try:
            run(['python3',source/'tests/posix/posix_2client.py','--local',mount,peer_mount,'--parent','gc-posix-peer','--results',work/'posix2.tsv'],timeout=600)
        except subprocess.CalledProcessError as error:
            if not a.continue_posix_failure:raise
            if posix_failure is None:posix_failure=error
            (work/'peer-failure.json').write_text(json.dumps({'returncode':error.returncode,'command':list(map(str,error.cmd))},indent=2))
            print('PEER FAILED; continuing independent gates, final status remains failure',flush=True)
        run([source/'scripts/client.sh','stop',peer_mount]);peer.wait(timeout=15);peer=None
        wait(drained,'POSIX server deletion queues drained',180)
        after_posix = totals(status())
        for metric in ('passes', 'removed_fragments', 'reclaimed_payload_bytes'):
            assert after_posix[metric] > posix_checkpoint[metric], (metric, posix_checkpoint, after_posix)
        (work/'posix-gc-effect.json').write_text(json.dumps({
            'before': posix_checkpoint, 'after': after_posix}, indent=2))
        print('POSIX deletes advance GC passes and physical removal counters PASS',flush=True)
    if a.append_burst:
        import append_burst
        burst_path,burst_report=append_burst.run(mount,peer_mount,a.append_burst,work)
        unmount();stop();start();mount_client()
        cold_hash=append_burst.verify(burst_path,a.append_burst)
        assert cold_hash==burst_report['warm_primary_sha256']
        burst_report['cold_sha256']=cold_hash
        (work/'append-burst.json').write_text(json.dumps(burst_report,indent=2))
        burst_path.unlink();wait(drained,'append burst physical deletion drained',180)
        print('append burst exact peer/cold blocks and final fsync PASS',flush=True)
    if a.append_audit:
        from collections import Counter
        path=mount/'append-cold-audit';path.touch()
        garbage=mount/'append-garbage';garbage.mkdir()
        for index in range(128):
            with (garbage/str(index)).open('xb',buffering=0) as out:
                out.write(bytes([index])*4096);os.fsync(out.fileno())
        before=totals(status());gate=threading.Barrier(5);errors=[]
        writers=4;count=200
        def record(writer,index):return f'W{writer:02d}S{index:04d}\n'.encode()
        def append_writer(writer):
            gate.wait()
            fd=os.open(path,os.O_WRONLY|os.O_APPEND) if writer%2 else None
            try:
                for index in range(count):
                    active_fd=fd if fd is not None else os.open(path,os.O_WRONLY|os.O_APPEND)
                    try:
                        body=record(writer,index)
                        assert os.write(active_fd,body)==len(body)
                        if index%13==0:os.fsync(active_fd)
                    finally:
                        if fd is None:os.close(active_fd)
                    # Keep appends active across several background GC passes.
                    time.sleep(.01)
                if fd is not None:os.fsync(fd)
            except BaseException as error:errors.append(repr(error))
            finally:
                if fd is not None:os.close(fd)
        def delete_during_append():
            gate.wait()
            for target in garbage.iterdir():target.unlink()
            garbage.rmdir()
        with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
            jobs=[pool.submit(append_writer,writer) for writer in range(writers)]
            jobs.append(pool.submit(delete_during_append))
            for job in jobs:job.result(timeout=120)
        assert not errors,errors
        warm=path.read_bytes();expected=Counter(record(writer,index) for writer in range(writers) for index in range(count))
        assert Counter(warm.splitlines(keepends=True))==expected,'warm append records lost/duplicated'
        after=totals(status());assert after['passes']>before['passes'],(before,after)
        unmount();stop();start();mount_client()
        cold=path.read_bytes();assert cold==warm,'cold append bytes changed'
        assert Counter(cold.splitlines(keepends=True))==expected,'cold append records lost/duplicated'
        evidence={'writers':writers,'records_per_writer':count,'exact_cold_records':writers*count,
                  'sha256':hashlib.sha256(cold).hexdigest(),'before_gc':before,'after_gc':after,
                  'mixed_per_record_close_and_persistent_descriptions':True}
        (work/'append-cold-audit.json').write_text(json.dumps(evidence,indent=2))
        path.unlink();wait(drained,'append audit physical deletion drained',180)
        print('mixed concurrent append/GC and exact 800 cold records PASS',flush=True)
    unmount();stop()
    start()
    # No user fragment remains, even on a restart. Physical scan is independent
    # of logical namespace disappearance and the asynchronous quota snapshot.
    wait(lambda:all(not [x for x in (root/'data/exports/1').glob('**/*') if x.is_file()] for root in data_roots),
         'physical inventory empty after restart',180)
    if a.wal_tail_recovery:
        mount_client()
        recovery_paths=[mount/'wal-before-tail',mount/'wal-after-tail']
        bodies=[hashlib.sha256(b'wal-prefix').digest()*4096,
                hashlib.sha256(b'wal-new-ack').digest()*4096]
        with recovery_paths[0].open('xb',buffering=0) as out:
            assert out.write(bodies[0])==len(bodies[0]);os.fsync(out.fileno())
        unmount();stop(crash=True)
        wals=sorted({path for root in roots for path in root.rglob('wal')})
        assert len(wals)==4,('expected one owned KV WAL per daemon',wals)
        evidence={'wals':[], 'acknowledged_bytes':list(map(len,bodies))}
        for path in wals:
            size=path.stat().st_size
            with path.open('ab',buffering=0) as out:
                assert out.write(bytes(8))==8;os.fsync(out.fileno())
            evidence['wals'].append({'path':str(path),'valid_bytes':size,'injected_tail_bytes':8})
        start();mount_client()
        assert recovery_paths[0].read_bytes()==bodies[0]
        with recovery_paths[1].open('xb',buffering=0) as out:
            assert out.write(bodies[1])==len(bodies[1]);os.fsync(out.fileno())
        # Let saved applied markers cover the acknowledged create/publication;
        # the next restart cannot rely only on replaying an older Raft index.
        time.sleep(1)
        unmount();stop(crash=True);start();mount_client()
        for path,body in zip(recovery_paths,bodies):
            assert path.read_bytes()==body,('post-tail cold data missing',path)
        evidence['cold_sha256']=[hashlib.sha256(path.read_bytes()).hexdigest() for path in recovery_paths]
        (work/'wal-tail-recovery.json').write_text(json.dumps(evidence,indent=2))
        for path in recovery_paths:path.unlink()
        wait(drained,'post-WAL-repair deletion queues drained')
        unmount();stop();start()
        wait(lambda:all(not [x for x in (root/'data/exports/1').glob('**/*') if x.is_file()] for root in data_roots),
             'post-WAL-repair physical inventory empty',180)
        print('invalid WAL tails repaired; later fsynced files survive second cold crash PASS',flush=True)
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
    if posix_failure:raise posix_failure
    print('GC reclamation '+a.mode+' PASS',flush=True)
finally:
    if a.storage_session_fence_fault:(work/'put-fence-armed').unlink(missing_ok=True)
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
