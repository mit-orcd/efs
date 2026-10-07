#!/usr/bin/env python3
"""Isolated four-node physical reclamation gate (Linux/FUSE; no existing stores).

Run on the NUC: python3 tests/live/gc_reclamation.py --mode direct
The two root parents must already exist. Only unique harness-owned directories
and processes are used; failed fixtures/logs are retained for diagnosis.
"""
import argparse
import ctypes
import struct
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--mode', choices=('buffered', 'direct'), required=True)
p.add_argument('--root', action='append', default=[])
p.add_argument('--posix', action='store_true')
p.add_argument('--integrity', action='store_true')
p.add_argument('--port', type=int, default=20190)
a = p.parse_args()
source = Path(__file__).resolve().parents[2]
parents = a.root or ['/data1/efs', '/data2/efs']
assert len(parents) == 2
for port in range(a.port, a.port+4):
    with socket.socket() as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(('127.0.0.1', port))
work = Path(tempfile.mkdtemp(prefix='gc-'+a.mode+'-', dir=parents[0]))
other = Path(tempfile.mkdtemp(prefix='gc-'+a.mode+'-', dir=parents[1]))
roots = [work/'n1',work/'n2',other/'n3',other/'n4']
mount = work/'mnt'
mount.mkdir()
digest=hashlib.sha256()
inputs=[source/'Makefile'] + [x for folder in ('src','include') for x in (source/folder).rglob('*') if x.suffix in ('.c','.h')]
for path in sorted(inputs):
    digest.update(str(path.relative_to(source)).encode()+b'\0'+path.read_bytes())
(work/"manifest.json").write_text(json.dumps({"mode":a.mode,"roots":[str(x) for x in roots],"port":a.port,"source_tree_sha256":digest.hexdigest()},indent=2))
env = dict(os.environ, EFS_MD_RAFT_N='4', EFS_TRANSPORT='tcp')
servers=[]
client=None
peer=None
peer_mount=work/"peer"
peer_mount.mkdir()
handles=[]
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
        cmd=[source/'efsd','--node-id',str(i+1),'--addr','127.0.0.1','--port',str(a.port+i),'--storage',root,'--quota','1G','--'+('direct-io' if a.mode=='direct' else 'no-direct-io')]
        if i:cmd += ['--join',f'127.0.0.1:{a.port}']
        servers.append(subprocess.Popen([str(x) for x in cmd],env=env,stdout=log,stderr=log))
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
    client=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{a.port}','default',str(mount),'-f'],env=env,stdout=log,stderr=log)
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
    for root in roots:
        folder=root/'data/exports/1'
        for part in reversed(seg):folder/=part
        found += [x for x in folder.glob('*/*') if x.is_file() and '.sum' not in x.name]
    return found

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

try:
    print('Evidence directory: '+str(work),flush=True)
    start()
    run([source/'efs-mgmt','raft-mkfs',f'127.0.0.1:{a.port}'])
    mount_client()
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
    # Two peers repeatedly touch adjacent ranges until the span chain folds.
    peer_log=open(work/'peer-fuse.log','ab');handles.append(peer_log)
    peer=subprocess.Popen([str(source/'efs-fuse'),f'127.0.0.1:{a.port}','default',str(peer_mount),'-f'],env=env,stdout=peer_log,stderr=peer_log)
    wait(lambda:subprocess.run(['mountpoint','-q',str(peer_mount)]).returncode==0,'peer mounted',40)
    fold=mount/'peer-fold';f1=os.open(fold,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
    os.write(f1,b'\0'*(128*1024));os.fsync(f1)
    f2=os.open(peer_mount/'peer-fold',os.O_RDWR)
    model=bytearray(128*1024)
    for i in range(20):
        fd=f1 if i%2==0 else f2
        off=0 if i%2==0 else 4096
        data=bytes([i+1])*4096
        assert os.pwrite(fd,data,off)==len(data);os.fsync(fd)
        model[off:off+len(data)]=data
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
    wait(lambda:all(not [x for x in (root/'data/exports/1').glob('**/*') if x.is_file()] for root in roots),
         'physical inventory empty after restart',180)
    wait(drained,'restart orphan and GC queues drained',180)
    final=status()
    assert all(node[1]['data_usage_bytes']==0 for node in final),final
    # A delayed valid-sized PUT must not resurrect the retired inode after
    # restart. The wire uses native C headers inside its big-endian framing.
    class Put(ctypes.Structure):
        _fields_=[('eid',ctypes.c_uint32),('ino',ctypes.c_uint64),('ci',ctypes.c_uint32),('fi',ctypes.c_uint32),('sum',ctypes.c_uint8*32),('length',ctypes.c_uint32),('gen',ctypes.c_uint64),('hint',ctypes.c_uint32)]
    def recv_exact(sock,n):
        body=b''
        while len(body)<n:
            part=sock.recv(n-len(body))
            assert part,'short reply'
            body+=part
        return body
    for i in range(4):
        req=Put(eid=1,ino=ino,ci=0,fi=0,length=65536,gen=987654321)
        payload=bytes(req)+bytes(65536)
        with socket.create_connection(('127.0.0.1',a.port+i),timeout=5) as sock:
            sock.sendall(struct.pack('>IB',len(payload)+1,6)+payload)
            length=struct.unpack('>I',recv_exact(sock,4))[0]
            reply=recv_exact(sock,length)
            assert reply[0]==7 and reply[1]==1,reply
    assert not files(ino)
    print('restart quota zero and late PUT fenced on every member PASS',flush=True)
    (work/'final-status.json').write_text(json.dumps(final,indent=2))
    print('physical fragment inventory empty after restart PASS',flush=True)
    print('GC reclamation '+a.mode+' PASS',flush=True)
finally:
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
