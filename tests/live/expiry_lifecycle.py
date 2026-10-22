"""Expiry live assertions used by the owned gc_reclamation fixture.

UID fields exercise the existing trusted-client identity model, not network
identity authentication. Raw observations stay in the fixture evidence directory.
"""
import errno
import json
import os
import socket
import subprocess
from pathlib import Path
import struct
import time


def exact(s,n):
    out=b''
    while len(out)<n:
        part=s.recv(n-len(out))
        if not part:raise AssertionError('short expiry RPC reply')
        out+=part
    return out

HELLO=struct.pack('>IB',17,165)+b'EFSP'+struct.pack('=IQ',4,0)  # CLIENT_HELLO, EFS_PROTOCOL_VERSION 4 (K5)
def declare(sock):
    sock.sendall(HELLO);n=struct.unpack('>I',exact(sock,4))[0];reply=exact(sock,n)
    assert n==9 and reply[0]==166 and struct.unpack('=iI',reply[1:])==(0,4),('declaration refused',reply)


def exercise(mount,port,work,wait,physical_files,restart,offline,forward_group,clock_fault=False):
    checks=[]
    def rpc(tag,ino=1,gen=1,extra=b'',accepted=(0,),raw=None):
        payload=raw if raw is not None else bytes((0 if ino&1 else 2,tag))+struct.pack('>QQ',ino,gen)+extra
        until=time.monotonic()+20
        while True:
            for node in range(4):
                with socket.create_connection(('127.0.0.1',port+node),timeout=15) as s:
                    declare(s);s.sendall(struct.pack('>IB',len(payload)+1,95)+payload)
                    n=struct.unpack('>I',exact(s,4))[0];reply=exact(s,n)
                assert reply[0]==96 and len(reply)==33,repr(reply)
                rc,hint,rev,value,other=struct.unpack('=iiQQQ',reply[1:])
                if rc in accepted:return rc,rev,value,other
                if rc not in (-13,-15):raise AssertionError((tag,ino,rc,hint))
            assert time.monotonic()<until,('expiry RPC did not progress',tag,ino,rc)
            time.sleep(.1)
    def policy(path,ttl=3,max_seconds=30,flags=3,uid=0,accepted=(0,)):
        ino=path.stat().st_ino
        _,rev,_,_=rpc(60,ino)
        return rpc(58,ino,extra=struct.pack('>IQQQB',uid,rev,int(ttl*1e9),int(max_seconds*1e9),flags),accepted=accepted)
    def record(path):return rpc(61,path.stat().st_ino)
    def renew(ino,rev,deadline,uid=None,accepted=(0,)):
        return rpc(59,ino,extra=struct.pack('>IQQQ',os.getuid() if uid is None else uid,rev,0,deadline),accepted=accepted)
    def enable(path,deadline,uid=None,gen=1,accepted=(0,)):
        ino=os.lstat(path).st_ino;name=path.name.encode()
        extra=struct.pack('>IQQQQB',os.getuid() if uid is None else uid,0,0,deadline,path.parent.stat().st_ino,len(name))+name
        return rpc(66,ino,gen,extra=extra,accepted=accepted)
    def created(path):
        with path.open('xb',buffering=0) as f:
            f.write(b'expiry payload '*400);os.fsync(f.fileno());ino=os.fstat(f.fileno()).st_ino
        return ino
    def mark(name):
        checks.append({'gate':name,'verdict':'PASS','wall_ns':time.time_ns()})
        (work/'expiry-lifecycle.json').write_text(json.dumps(checks,indent=2));print('expiry '+name+' PASS',flush=True)
    base=mount/'expiry-gates';base.mkdir()
    plain=base/'default-off';created(plain);assert record(plain)[1:]==(0,0,0);mark('default-off')
    policy(base,uid=os.getuid(),accepted=(-16,));mark('operator authorization')
    cli=Path(__file__).resolve().parents[2]/'efs-mgmt'
    policy(base,ttl=30,max_seconds=90)
    inspected=base/'inspection';created(inspected)
    for target in (inspected,base):
        result=subprocess.run([str(cli),'expiry',f'127.0.0.1:{port}','show',str(target),'--mount',str(mount)],capture_output=True,text=True,check=True)
        assert 'policy_source: inode' in result.stdout,result.stdout
        assert ('expires in' if target==inspected else 'future_file_policy: enabled') in result.stdout,result.stdout
    inspected.unlink();policy(base,ttl=3)
    result=subprocess.run([str(cli),'expiry',f'127.0.0.1:{port}','show','/missing-expiry-inspection'],capture_output=True,text=True)
    assert result.returncode!=0
    mark('path inspection dates and inherited folder policy')
    info=subprocess.run([str(Path(__file__).resolve().parents[2]/'efs-mgmt'),'expiry-id',f'127.0.0.1:{port}',str(base.stat().st_ino)],capture_output=True,text=True,check=True)
    assert 'generation=1' in info.stdout and 'nlink=2' in info.stdout,info.stdout
    rpc(60,base.stat().st_ino,gen=2,accepted=(-14,))
    rpc(61,plain.stat().st_ino,gen=2,accepted=(-14,))
    mark('authoritative FileID inspection and stale generation reads')
    policy(base,ttl=4)
    born=base/'birth';ino=created(born);_,rev,deadline,root=record(born)
    assert rev==1 and deadline>time.time_ns() and root==base.stat().st_ino
    policy(base,ttl=12);assert record(born)[2]==deadline;mark('fixed birth and future-only policy')
    wait(lambda:not born.exists(),'expiry removes eligible birth',40)
    wait(lambda:not physical_files(ino),'expiry physical reclamation',90);mark('namespace and physical reclamation')
    # Force inheritance across the two actual metadata groups, rather than
    # relying on whatever name hashing the seed happens to choose. Nested
    # mkdir keeps the parent's group (mkdir group affinity), so a child of
    # base never crosses groups. Root-level directories are scattered: make
    # one in the other group and rename it under base; the inode keeps it.
    directories=[];cross=None
    for i in range(32):
        candidate=mount/('cross-policy-'+str(i));candidate.mkdir()
        if (candidate.stat().st_ino&1)!=(base.stat().st_ino&1):
            cross=base/'cross-policy';candidate.rename(cross);directories.append(cross);break
        directories.append(candidate)
    assert cross is not None,'fixture failed to choose a different inode group'
    cross_id=subprocess.run([str(Path(__file__).resolve().parents[2]/'efs-mgmt'),'expiry-id',f'127.0.0.1:{port}',str(cross.stat().st_ino)],capture_output=True,text=True,check=True)
    assert 'generation=1' in cross_id.stdout,cross_id.stdout
    (work/'expiry-fileid-cli.txt').write_text(info.stdout+cross_id.stdout)
    inherited=cross/'inherited';cross_ino=created(inherited)
    assert (cross_ino&1)!=(base.stat().st_ino&1)
    assert record(inherited)[3]==base.stat().st_ino
    wait(lambda:not inherited.exists(),'cross-group inherited expiry',40)
    wait(lambda:not physical_files(cross_ino),'cross-group inherited reclamation',90)
    for directory in directories:directory.rmdir()
    mark('explicit cross-group inheritance and deletion')
    held=base/'held';policy(base,ttl=3);ino=created(held)
    fd=os.open(held,os.O_RDWR)
    try:
        wait(lambda:not held.exists(),'open expiry namespace removal',40)
        assert os.pread(fd,14,0)==b'expiry payload'
        os.pwrite(fd,b'held',0);os.fsync(fd)
        assert os.pread(fd,4,0)==b'held'
        assert physical_files(ino)
    finally:os.close(fd)
    wait(lambda:not physical_files(ino),'expired last-close reclamation',90);mark('open descriptor retained then reclaimed')
    policy(base,ttl=4)
    path=base/'renewed';ino=created(path);_,rev,deadline,_=record(path)
    renew(ino,rev,time.time_ns()+int(12e9),uid=os.getuid()+1,accepted=(-16,))
    renew(ino,rev,time.time_ns()+int(12e9));assert record(path)[1]==rev+1
    renew(ino,rev,0,accepted=(-14,));time.sleep(max(0,(deadline-time.time_ns())/1e9)+1)
    assert path.exists()
    name=path.name.encode();due=bytes((0 if ino&1 else 2,65))+struct.pack('>QQQQQB',base.stat().st_ino,ino,1,rev,deadline,len(name))+name
    # Replay the actual older deletion request twice after eligibility. It
    # must fail its replicated record CAS rather than delete the renewed file.
    for _ in range(2):rpc(65,ino,accepted=(-14,),raw=due)
    assert path.exists();mark('owner renewal invalidates old deadline and deletion replay')
    try:os.link(path,base/'forbidden-link')
    except OSError as e:assert e.errno in (errno.EPERM,errno.EACCES,errno.EINVAL,errno.EOPNOTSUPP),e
    else:raise AssertionError('active expiry hardlink accepted')
    mark('active hardlink refusal')
    _,rev,_,_=record(path);renew(ino,rev,0);assert record(path)[2]==0
    # Clearing permits normal hard-link lifetime. Reactivation must not use
    # a deleted old name to schedule deletion of a different binding.
    alias=base/'cleared-alias';os.link(path,alias);_,rev,_,_=record(path)
    renew(ino,rev,time.time_ns()+int(10e9),accepted=(-5,))
    path.unlink();renew(ino,rev,time.time_ns()+int(10e9),accepted=(-3,-14))
    assert alias.exists();alias.rename(path)
    mark('cleared-link and stale binding refusal')
    policy(base,ttl=3)
    moving=base/'moving';ino=created(moving);_,rev,deadline,_=record(moving)
    dest=base/'renamed';moving.rename(dest);assert record(dest)[2]==deadline
    replacement=created(moving);_,replacement_rev,_,_=record(moving);renew(replacement,replacement_rev,0)
    wait(lambda:not dest.exists(),'renamed expiry',40)
    assert moving.exists() and moving.read_bytes()==b'expiry payload '*400
    moving.unlink()
    wait(lambda:not physical_files(ino) and not physical_files(replacement),'rename physical reclaim',90);mark('rename binding and preserved path replacement')
    child=base/'disabled';child.mkdir();policy(child,ttl=0,flags=0)
    survivor=child/'survivor';created(survivor);assert record(survivor)[2]==0;mark('disabled subtree shadows ancestor')
    policy(base,ttl=3,flags=1)
    restricted=base/'restricted';ino=created(restricted);_,rev,_,_=record(restricted)
    renew(ino,rev,0,accepted=(-16,));renew(ino,rev,time.time_ns()+int(60e9),accepted=(-5,))
    wait(lambda:not restricted.exists(),'restricted expiry',40);mark('clear and retention policy enforced')
    policy(base,ttl=3)
    alias=base/'legacy-alias';os.link(plain,alias)
    enable(plain,time.time_ns()+int(4e9),accepted=(-5,));assert record(plain)[1]==0;alias.unlink()
    enable(plain,time.time_ns()+int(4e9),uid=os.getuid()+1,accepted=(-16,))
    enable(plain,time.time_ns()+int(4e9),gen=2,accepted=(-14,))
    enable(plain,time.time_ns()+int(60e9),accepted=(-5,))
    legacy_ino=plain.stat().st_ino;enable(plain,time.time_ns()+int(4e9))
    assert record(plain)[1]==1;enable(plain,time.time_ns()+int(4e9),accepted=(-4,))
    wait(lambda:not plain.exists(),'explicit existing-file expiry',40)
    wait(lambda:not physical_files(legacy_ino),'existing-file physical reclaim',90)
    enable(survivor,time.time_ns()+int(4e9),accepted=(-16,));assert record(survivor)[1]==0
    link=base/'symlink';os.symlink('renewed',link)
    enable(link,time.time_ns()+int(4e9),accepted=(-5,));link.unlink()
    mark('explicit existing-file expiry and link/generation/policy/type refusals')
    raced=base/'publication';ino=created(raced);fd=os.open(raced,os.O_RDWR)
    try:
        deadline=time.monotonic()+7;writes=0
        while time.monotonic()<deadline:
            assert os.pwrite(fd,b'concurrent '+bytes([writes%256]),0)==12
            os.fsync(fd);writes+=1;time.sleep(.03)
        assert not raced.exists() and os.pread(fd,11,0)==b'concurrent '
    finally:os.close(fd)
    wait(lambda:not physical_files(ino),'concurrent publication reclaimed',90);mark('publication concurrent with expiry')
    cold=base/'cold';ino=created(cold);restart()
    wait(lambda:not cold.exists(),'cold restart expiry',40)
    wait(lambda:not physical_files(ino),'cold expiry physical reclaim',90);mark('durable index cold restart')
    paused_feature=base/'feature-off-existing';ino=created(paused_feature)
    restart(False);time.sleep(5);assert paused_feature.exists()
    no_birth=base/'feature-off-birth';created(no_birth)
    restart(True)
    wait(lambda:not paused_feature.exists(),'re-enabled index catches overdue deadline',40)
    wait(lambda:not physical_files(ino),'feature re-enable physical reclaim',90)
    assert no_birth.exists() and record(no_birth)[1:]==(0,0,0)
    no_birth.unlink();mark('global disable pauses and bounded re-enable catches up')
    remote=base/'offline';ino=created(remote);rejoin=offline(storage_ino=ino)
    try:
        wait(lambda:not remote.exists(),'expiry with offline storage member',40)
        assert physical_files(ino),'offline physical debt unexpectedly gone'
    finally:rejoin()
    wait(lambda:not physical_files(ino),'offline expiry reclamation after rejoin',90);mark('offline storage retains debt until rejoin')
    group=forward_group();coordinator_dirs=[];coordinator_dir=None
    # As above: nested mkdir keeps base's group, so pick a scattered
    # root-level directory in the forwarding group and move it under base.
    for i in range(32):
        directory=mount/('coordinator-'+str(i));directory.mkdir()
        if (0 if directory.stat().st_ino&1 else 2)==group:
            coordinator_dir=base/'coordinator';directory.rename(coordinator_dir)
            coordinator_dirs.append(coordinator_dir);break
        coordinator_dirs.append(directory)
    assert coordinator_dir is not None
    routed=coordinator_dir/'routed';ino=created(routed);rejoin=offline(node_id=2)
    try:wait(lambda:not routed.exists(),'expiry progresses with first coordinator offline',45)
    finally:rejoin()
    wait(lambda:not physical_files(ino),'coordinator-loss physical reclaim',90)
    for directory in coordinator_dirs:directory.rmdir()
    mark('first dual-group coordinator loss and surviving routing')
    elected=base/'leader-change';ino=created(elected)
    rejoin=offline(0 if ino&1 else 2)
    try:wait(lambda:not elected.exists(),'expiry after actual metadata leader loss',45)
    finally:rejoin()
    wait(lambda:not physical_files(ino),'leader-loss expiry physical reclaim',90)
    mark('metadata leader loss and election')
    if clock_fault:
        marker=work/'expiry-clock-uncertain';marker.touch()
        def clock(node,reset=False):
            payload=bytes((0,64 if reset else 63))+(struct.pack('>I',0) if reset else b'')
            with socket.create_connection(('127.0.0.1',port+node),timeout=15) as sock:
                declare(sock);sock.sendall(struct.pack('>IB',len(payload)+1,95)+payload)
                n=struct.unpack('>I',exact(sock,4))[0];reply=exact(sock,n)
            rc,_,bits,_,_=struct.unpack('=iiQQQ',reply[1:]);assert rc==0
            return bits
        wait(lambda:all(clock(n)&2 for n in range(4)),'uncertain clock latched',30)
        state=subprocess.check_output([str(cli),'expiry-status',f'127.0.0.1:{port}'],text=True)
        assert 'paused: clock uncertain' in state and 'operator reset required' in state,state
        paused=base/'clock-paused';ino=created(paused);time.sleep(5);assert paused.exists()
        marker.unlink();time.sleep(2);assert paused.exists(),'uncertainty cleared without explicit reset'
        for n in range(4):clock(n,True)
        wait(lambda:not paused.exists(),'clock reset resumes expiry',40)
        wait(lambda:not physical_files(ino),'clock-resumed physical reclaim',90);mark('uncertain clock pauses until operator reset')
    policy(base,ttl=3)
    bulk=base/'bulk';bulk.mkdir();inos=[created(bulk/str(i)) for i in range(144)]
    wait(lambda:not any(bulk.iterdir()),'paged deadline backlog drained',100)
    wait(lambda:all(not physical_files(i) for i in inos),'bulk expiry physical reclamation',120)
    bulk.rmdir();mark('144-file bounded page backlog')
    statuses=[subprocess.check_output([str(cli),'gc-status',f'127.0.0.1:{port+i}'],text=True) for i in range(4)]
    import re
    removed=sum(int(re.search(r'expired_files=(\d+)',text).group(1)) for text in statuses)
    logical=sum(int(re.search(r'expired_namespace_bytes=(\d+)',text).group(1)) for text in statuses)
    assert removed>=144 and logical>=144*len(b'expiry payload '*400),(removed,logical,statuses)
    assert all('expiry_available=1' in text and 'scope=since_process_start' in text for text in statuses)
    (work/'expiry-worker-status.txt').write_text('\n'.join(statuses))
    mark('worker status confirmed removals and logical bytes')
    # Keep ordinary regression tests outside this policy, remove only owned tree.
    path.unlink();survivor.unlink();child.rmdir();base.rmdir()
    mark('owned tree cleanup')
