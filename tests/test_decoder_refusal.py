#!/usr/bin/env python3
"""Task C/W140: refusal on three real private loopback daemons, no FUSE.

Unknown declared messages and bad declarations disconnect with an audible
reason. Unknown submit tags are refused through leader and follower RPCs;
the log does not move, no group halts, a subsequent real CREATE commits.
"""
from pathlib import Path
import os
import re
import socket
import struct
import subprocess
import tempfile
import time
import k33_daemons as fixture
ROOT=Path(__file__).resolve().parents[1]
fixture.EFSD=ROOT/'efsd'
work=Path(tempfile.mkdtemp(prefix='efs-decoder-refusal-',dir=os.environ.get('TMPDIR')))
env={k:v for k,v in os.environ.items() if not k.startswith('EFS_FAULT_')}
c=fixture.Cluster(work,'refusal',(1,2,3),env)
def frame(s,tag,body=b''):
    s.sendall(struct.pack('!IB',1+len(body),tag)+body)
def exact(s,n):
    b=b''
    while len(b)<n:
        got=s.recv(n-len(b))
        assert got,'unexpected disconnect'
        b+=got
    return b
def recv(s):
    n,=struct.unpack('!I',exact(s,4)); assert 1<=n<=65536
    body=exact(s,n);return body[0],body[1:]
def connect(i,version=5):
    s=socket.create_connection((fixture.HOST,c.ports[i]),timeout=5)
    # Native structure ABI, exactly as efs_msg_client_hello on this host.
    frame(s,165,struct.pack('@4sIQ',b'EFSP',version,0))
    tag,body=recv(s);assert tag==166
    rc,served=struct.unpack('@iI',body);assert served==5
    return s,rc
def status(i):
    rc,text=fixture.mgmt(env,'raft-status',c.ep(i));assert not rc,text
    rows={int(g):(int(commit),int(applied)) for g,commit,applied in
          re.findall(r'group (\d+).*?commit=(\d+) applied=(\d+)',text)}
    assert set(rows)=={0,2},text
    return rows
try:
    c.start(1);c.start(2,join=1);c.start(3,join=1)
    c.wait_voters((1,2,3),lambda i,s: all(s.get(g)==(1,7) for g in (0,2)))
    # Formation may still be publishing its salt: require ReadIndex at both
    # groups before checking that refusal adds no entry.
    for g in (0,2):
        ready=False
        deadline=time.monotonic()+60
        while time.monotonic()<deadline:
            for i in (1,2,3):
                with connect(i)[0] as s:
                    frame(s,95,bytes([g]));tag,body=recv(s)
                    assert tag==96,(tag,body)
                    if struct.unpack_from('@i',body)[0]==0:
                        ready=True
                        break
            if ready:break
            time.sleep(.1)
        assert ready,('group never ReadIndex-ready',g)
    for i in (1,2,3):
        with connect(i,version=0)[0] as s:
            assert s.recv(1)==b''
        with connect(i)[0] as s:
            frame(s,255);assert s.recv(1)==b''
        before=status(i)
        for g in (0,2):
            with connect(i)[0] as s:
                frame(s,95,bytes([g,200]));tag,body=recv(s)
                assert tag==96 and struct.unpack_from('@i',body)[0]==-7,(tag,body)
        after=status(i);assert before==after,(i,before,after)
        assert c.alive(i)
        c.wait_log(i,'handler: refused declaration of protocol 0')
        c.wait_log(i,'handler: refused unknown message 255')
    rc,text=fixture.mgmt(env,'raft-create',c.ep(1),'1','after-refusal')
    assert not rc and 'status=0' in text,text
    logs='\n'.join(c.log(i) for i in (1,2,3))
    assert 'refused proposal tag=200' in logs
    assert 'halt' not in logs.lower(),logs[-3000:]
    print('PASS W140 cluster submit refusal: leader/follower, both groups, no log entry, subsequent CREATE commits')
    print('PASS live declaration/unknown-frame disconnect and diagnostics on all three nodes')
finally:
    c.stop_all()
    # Keep the test evidence when failed; callers archive text and remove
    # their private parent tree with edelete after recording conclusions.
    print('Private refusal evidence:',work)
