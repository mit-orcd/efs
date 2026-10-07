#!/usr/bin/env python3
"""Private daemon RPC check: store read and GC request/removal counters."""
import ctypes as c
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parents[2]
class Put(c.Structure):
    _fields_ = [('eid', c.c_uint32), ('ino', c.c_uint64), ('ci', c.c_uint32),
                ('fi', c.c_uint32), ('sum', c.c_ubyte*32), ('length', c.c_uint32),
                ('gen', c.c_uint64), ('hint', c.c_uint32)]
class Get(c.Structure):
    _fields_ = [('eid', c.c_uint32), ('ino', c.c_uint64), ('ci', c.c_uint32),
                ('fi', c.c_uint32), ('gen', c.c_uint64)]
class Delete(c.Structure):
    _fields_ = [('eid', c.c_uint32), ('ino', c.c_uint64), ('ci', c.c_uint32),
                ('fi', c.c_uint32), ('sum', c.c_ubyte*32), ('gen', c.c_uint64)]

def exact(sock, length):
    result = b''
    while len(result) < length:
        part = sock.recv(length-len(result))
        assert part, 'short reply'
        result += part
    return result

with socket.socket() as sock:
    sock.bind(('127.0.0.1', 0))
    port = sock.getsockname()[1]

def rpc(kind, body):
    with socket.create_connection(('127.0.0.1', port), 5) as sock:
        sock.sendall(struct.pack('>IB', len(body)+1, kind)+body)
        return exact(sock, struct.unpack('>I', exact(sock, 4))[0])

def status(command):
    return subprocess.check_output([str(root/'efs-mgmt'), command,
                                    f'127.0.0.1:{port}'], text=True)

with tempfile.TemporaryDirectory(prefix='efs-io-stats-') as work:
    with open(work+'/daemon.log', 'w+') as log:
        proc = subprocess.Popen([str(root/'efsd'), '--node-id', '1', '--addr',
                                 '127.0.0.1', '--port', str(port), '--storage', work,
                                 '--no-direct-io'], env=dict(os.environ,EFS_MD_RAFT_N='3'),
                                stdout=log, stderr=log)
        try:
            for _ in range(100):
                assert proc.poll() is None, 'daemon exited'
                try:
                    with socket.create_connection(('127.0.0.1',port), .1): break
                except OSError: time.sleep(.1)
            else: raise AssertionError('daemon startup timeout')
            put=Put(eid=1,ino=99,length=65536,gen=12345)
            # BLAKE3 of 65536 bytes of 'x', computed with EFS's checksum implementation.
            checksum=bytes.fromhex('70ff942c316810ac5ffe7081fc049dba30713646b1d6d7272cd5948ad5579804')
            put.sum[:]=checksum
            assert rpc(6, bytes(put)+b'x'*65536)[:2] == bytes([7,0])
            get=Get(eid=1,ino=99,gen=12345)
            reply=rpc(4,bytes(get))
            assert reply[:2] == bytes([5,0]) and reply[34:] == b'x'*65536, (reply[:40],len(reply))
            delete=Delete(eid=1,ino=99,gen=12345)
            delete.sum[:]=checksum
            for _ in range(2): assert rpc(99,bytes(delete))[:2] == bytes([100,0])
            assert rpc(4,bytes(get))[:2] == bytes([5,1])
            io=status('io-stats'); gc=status('gc-status')
            rows={}
            for line in io.splitlines()[1:]:
                rows[line.split()[0]]={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)}
            assert rows['disk_read']['ops']==2 and rows['disk_read']['bytes']==65536
            assert rows['disk_read']['errors']==1 and rows['gc_delete']['ops']==2
            assert 'removed_fragments=1 ' in gc and 'reclaimed_payload_bytes=65536 ' in gc
            print(io+gc)
            print('I/O stats: real PUT/GET, missing read, GC retry and single physical removal PASS')
        finally:
            proc.terminate()
            try: proc.wait(15)
            except subprocess.TimeoutExpired: proc.kill();proc.wait()
