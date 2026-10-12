"""Expiry live assertions used by the owned gc_reclamation fixture.

UID fields exercise the existing trusted-client identity model, not network
identity authentication. Raw observations stay in the fixture evidence directory.
"""
import errno
import json
import os
import socket
import struct
import time


def exact(s,n):
    out=b''
    while len(out)<n:
        part=s.recv(n-len(out))
        if not part:raise AssertionError('short expiry RPC reply')
        out+=part
    return out


def exercise(mount,port,work,wait,physical_files,restart,offline,clock_fault=False):
    checks=[]
    def rpc(tag,ino=1,gen=1,extra=b'',accepted=(0,)):
        payload=bytes((0 if ino&1 else 2,tag))+struct.pack('>QQ',ino,gen)+extra
        until=time.monotonic()+20
        while True:
            for node in range(4):
                with socket.create_connection(('127.0.0.1',port+node),timeout=15) as s:
                    s.sendall(struct.pack('>IB',len(payload)+1,95)+payload)
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
    policy(base,ttl=4)
    born=base/'birth';ino=created(born);_,rev,deadline,root=record(born)
    assert rev==1 and deadline>time.time_ns() and root==base.stat().st_ino
    policy(base,ttl=12);assert record(born)[2]==deadline;mark('fixed birth and future-only policy')
    wait(lambda:not born.exists(),'expiry removes eligible birth',40)
    wait(lambda:not physical_files(ino),'expiry physical reclamation',90);mark('namespace and physical reclamation')
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
    assert path.exists();mark('owner renewal invalidates old deadline')
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
    replacement=created(moving)
    wait(lambda:not dest.exists(),'renamed expiry',40);wait(lambda:not moving.exists(),'replacement own expiry',40)
    wait(lambda:not physical_files(ino) and not physical_files(replacement),'rename physical reclaim',90);mark('rename binding and path reuse')
    child=base/'disabled';child.mkdir();policy(child,ttl=0,flags=0)
    survivor=child/'survivor';created(survivor);assert record(survivor)[2]==0;mark('disabled subtree shadows ancestor')
    policy(base,ttl=3,flags=1)
    restricted=base/'restricted';ino=created(restricted);_,rev,_,_=record(restricted)
    renew(ino,rev,0,accepted=(-16,));renew(ino,rev,time.time_ns()+int(60e9),accepted=(-5,))
    wait(lambda:not restricted.exists(),'restricted expiry',40);mark('clear and retention policy enforced')
    policy(base,ttl=3)
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
    remote=base/'offline';ino=created(remote);rejoin=offline()
    try:
        wait(lambda:not remote.exists(),'expiry with offline storage member',40)
        assert physical_files(ino),'offline physical debt unexpectedly gone'
    finally:rejoin()
    wait(lambda:not physical_files(ino),'offline expiry reclamation after rejoin',90);mark('offline storage retains debt until rejoin')
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
                sock.sendall(struct.pack('>IB',len(payload)+1,95)+payload)
                n=struct.unpack('>I',exact(sock,4))[0];reply=exact(sock,n)
            rc,_,bits,_,_=struct.unpack('=iiQQQ',reply[1:]);assert rc==0
            return bits
        wait(lambda:all(clock(n)&2 for n in range(4)),'uncertain clock latched',30)
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
    # Keep ordinary regression tests outside this policy, remove only owned tree.
    path.unlink();plain.unlink();survivor.unlink();child.rmdir();base.rmdir()
    mark('owned tree cleanup')
