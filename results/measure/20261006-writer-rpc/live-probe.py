import os,socket,struct,tempfile
base='/data1/efs/review-20261006-writer-rpc'
def exact(s,n):
    b=b''
    while len(b)<n:
        v=s.recv(n-len(b))
        if not v: raise RuntimeError('short frame')
        b+=v
    return b
def request(port,ino,gen,ci,payload=None):
    payload=payload if payload is not None else struct.pack('@QQII',ino,gen,ci,0)
    with socket.create_connection(('127.0.0.1',port),timeout=15) as s:
        s.sendall(struct.pack('!I',1+len(payload))+bytes([107])+payload)
        n=struct.unpack('!I',exact(s,4))[0]; frame=exact(s,n)
    assert frame[0]==108 and len(frame)==569,(frame[0],len(frame))
    return struct.unpack_from('@B3xIQQIIQQI',frame,1)
checks=0
directory=tempfile.mkdtemp(prefix='writer-rpc-',dir=base+'/mnt')
for i in range(8):
    path=f'{directory}/file-{i}'
    with open(path,'wb'): pass
    ino=os.stat(path).st_ino
    generation=None
    for port in (20432,20433,20434):
        for ci in (0,17,63,64):
            r=request(port,ino,0,ci)
            assert r[0]==0 and r[2]==ino and r[3]>0 and r[4]==ci and r[6:]==(0,0,0),r
            generation=r[3]
            assert request(port,ino,generation+1,ci)[0]==11,'wrong FileID accepted'
            checks+=2
    with open(path,'wb') as f:
        f.write(b'writer snapshot smoke\n'); f.flush(); os.fsync(f.fileno())
    for port in (20432,20433,20434):
        r=request(port,ino,generation,0)
        assert r[0]==0 and r[6:]==(0,0,0),r
        checks+=1
    os.unlink(path)
os.rmdir(directory)
assert request(20432,0,0,0)[0]==6
assert request(20432,1,0,0,payload=b'x')[0]==6
print(f'writer RPC live: {checks} hole, FileID and published-row checks PASS across all three nodes; malformed requests rejected')
