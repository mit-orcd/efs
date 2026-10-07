#!/usr/bin/env python3
"""Private Linux RPC corruption gate for recorded fragment integrity evidence."""
import ctypes as c
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

source = Path(__file__).resolve().parents[2]
class Put(c.Structure):
    _fields_ = [('eid', c.c_uint32), ('ino', c.c_uint64), ('ci', c.c_uint32),
                ('fi', c.c_uint32), ('sum', c.c_ubyte*32), ('length', c.c_uint32),
                ('gen', c.c_uint64), ('hint', c.c_uint32)]
class Get(c.Structure):
    _fields_ = [('eid', c.c_uint32), ('ino', c.c_uint64), ('ci', c.c_uint32),
                ('fi', c.c_uint32), ('gen', c.c_uint64)]

def exact(sock, length):
    result = b''
    while len(result) < length:
        part = sock.recv(length-len(result))
        assert part, 'short reply'
        result += part
    return result

for direct in (False, True):
    for roots in (1, 2):
        with tempfile.TemporaryDirectory(prefix='efs-integrity-') as work:
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
            def rpc(kind, body):
                with socket.create_connection(('127.0.0.1', port), 5) as sock:
                    sock.sendall(struct.pack('>IB',len(body)+1,kind)+body)
                    return exact(sock,struct.unpack('>I',exact(sock,4))[0])
            cmd=[str(source/'efsd'),'--node-id','1','--addr','127.0.0.1',
                 '--port',str(port),'--direct-io' if direct else '--no-direct-io']
            for index in range(roots):
                root=Path(work)/f'root{index}';root.mkdir()
                cmd += ['--storage',str(root)]
            with open(work+'/daemon.log','w+') as log:
                proc=subprocess.Popen(cmd,env=dict(os.environ,EFS_MD_RAFT_N='3'),stdout=log,stderr=log)
                try:
                    for _ in range(100):
                        assert proc.poll() is None, 'daemon exited'
                        try:
                            with socket.create_connection(('127.0.0.1',port),.1):break
                        except OSError:time.sleep(.1)
                    else:raise AssertionError('startup timeout')
                    put=Put(eid=1,ino=99,length=65536,gen=12345)
                    put.sum[:]=bytes.fromhex('70ff942c316810ac5ffe7081fc049dba30713646b1d6d7272cd5948ad5579804')
                    assert rpc(6,bytes(put)+b'x'*65536)[:2] == bytes([7,0])
                    get=Get(eid=1,ino=99,gen=12345)
                    def good():
                        reply=rpc(4,bytes(get))
                        assert reply[:2]==bytes([5,0]) and reply[34:]==b'x'*65536,reply[:40]
                    def bad(label):
                        reply=rpc(4,bytes(get))
                        assert reply==bytes([5,1]),(label,reply[:40])
                        print(f'{direct=} {roots=} {label}: unavailable PASS',flush=True)
                    good()
                    candidates=[path for root in Path(work).glob('root*') for path in (root/'data/exports/1').rglob('*') if path.is_file()]
                    assert len(candidates)==1,candidates
                    path=candidates[0];original=path.read_bytes()
                    with path.open('r+b') as fragment:fragment.write(b'y')
                    bad('payload flip')
                    path.write_bytes(original);good()
                    with path.open('r+b') as fragment:fragment.truncate(65536)
                    bad('missing digest')
                    path.write_bytes(original[:65536]+original[65536:65544])
                    bad('truncated digest')
                    path.write_bytes(original)
                    with path.open('r+b') as fragment:fragment.seek(65536);fragment.write(b'!')
                    bad('digest flip')
                    path.write_bytes(original);good()
                finally:
                    proc.terminate()
                    try:proc.wait(20)
                    except subprocess.TimeoutExpired:proc.kill();proc.wait()
print('Fragment corruption: buffered/direct, one/two roots PASS',flush=True)
