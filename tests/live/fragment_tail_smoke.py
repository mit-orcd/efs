#!/usr/bin/env python3
"""Bounded actual GET smoke: direct reader accepts both stored digest tails.

Usage: python3 tests/live/fragment_tail_smoke.py [BINARY_DIRECTORY]
Starts one temporary TCP/direct files daemon; never accesses a deployed store.
"""
import ctypes as c
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

source = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]

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

HELLO=struct.pack('>IB',17,165)+b'EFSP'+struct.pack('=IQ',5,0)  # CLIENT_HELLO, EFS_PROTOCOL_VERSION 5 (K13)
def declare(sock):
    sock.sendall(HELLO);n=struct.unpack('>I',exact(sock,4))[0];reply=exact(sock,n)
    assert n==9 and reply[0]==166 and struct.unpack('=iI',reply[1:])==(0,5),('declaration refused',reply)

with tempfile.TemporaryDirectory(prefix='efs-tail-smoke-') as work:
    root = Path(work)/'store'
    root.mkdir()
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]

    def rpc(kind, body):
        with socket.create_connection(('127.0.0.1', port), 5) as sock:
            sock.settimeout(5)
            declare(sock);sock.sendall(struct.pack('>IB', len(body)+1, kind)+body)
            return exact(sock, struct.unpack('>I', exact(sock, 4))[0])

    env = dict(os.environ, EFS_TRANSPORT='tcp', EFS_COMMANDS='0',
               EFS_COMMANDS_SERVER='0', EFS_EXPIRY='0',
               EFS_MD_RAFT_N='3')
    with (Path(work)/'daemon.log').open('w+') as log:
        proc = subprocess.Popen([str(source/'efsd'), '--node-id', '1',
            '--addr', '127.0.0.1', '--port', str(port), '--storage', str(root),
            '--quota', '16M', '--direct-io'],
            env=env, stdout=log, stderr=log)
        try:
            for _ in range(100):
                assert proc.poll() is None, 'daemon exited'
                try:
                    with socket.create_connection(('127.0.0.1', port), .1):
                        break
                except OSError:
                    time.sleep(.1)
            else:
                raise AssertionError('startup timeout')
            put = Put(eid=1, ino=99, length=65536, gen=12345)
            put.sum[:] = bytes.fromhex('70ff942c316810ac5ffe7081fc049dba30713646b1d6d7272cd5948ad5579804')
            assert rpc(6, bytes(put)+b'x'*65536)[:2] == bytes([7, 0])
            get = Get(eid=1, ino=99, gen=12345)
            fragment = next((root/'data/exports/1').rglob('0.0.12345'))
            original = fragment.read_bytes()
            assert len(original) == 65536+4096

            def check(label, accepted):
                reply = rpc(4, bytes(get))
                if accepted:
                    assert reply[:2] == bytes([5, 0]) and reply[2:34] == bytes(put.sum) and reply[34:] == b'x'*65536, (label, reply[:40])
                else:
                    assert reply == bytes([5, 1]), (label, reply[:40])
                print(label, 'PASS', flush=True)

            check('direct padded tail', True)
            for length in (32, 4096):
                with fragment.open('r+b') as f:
                    f.write(original[:65536+length])
                    f.truncate(65536+length)
                    f.flush()
                    os.fsync(f.fileno())
                check('stored tail length %d' % length, True)
            for length in (31, 0):
                with fragment.open('r+b') as f:
                    f.truncate(65536+length)
                    f.flush()
                    os.fsync(f.fileno())
                check('incomplete tail length %d refused' % length, False)
            for corruption in ('payload', 'digest'):
                data = bytearray(original[:65536+32])
                data[0 if corruption == 'payload' else 65536] ^= 1
                with fragment.open('wb') as f:
                    f.write(data)
                    f.flush()
                    os.fsync(f.fileno())
                check('corrupt %s refused' % corruption, False)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
print('direct GET stored-tail smoke PASS')
