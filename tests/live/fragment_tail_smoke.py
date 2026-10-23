#!/usr/bin/env python3
"""Bounded actual GET smoke: direct reader accepts both stored record tails
(K16: the 128-byte fragment record, or the 4 KiB direct page holding it).

Usage: python3 tests/live/fragment_tail_smoke.py [BINARY_DIRECTORY]
Starts a temporary formed three-member TCP/direct files cluster; never
accesses a deployed store. K13 (W158): the fragment is stored under an
ADMITTED ticket (tests/live/ticket_put.py); an unticketed PUT is refused.
"""
import ctypes as c
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile

repo = Path(__file__).resolve().parents[2]
source = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else repo
sys.path.insert(0, str(repo/'tests/live'))
import ticket_put

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
    env = dict(os.environ, EFS_COMMANDS='0', EFS_COMMANDS_SERVER='0', EFS_EXPIRY='0')
    cluster = ticket_put.Cluster(source, work, extra=['--quota', '16M', '--direct-io'], env=env)

    def rpc(node, kind, body):
        with socket.create_connection(('127.0.0.1', cluster.base+node), 5) as sock:
            sock.settimeout(5)
            declare(sock);sock.sendall(struct.pack('>IB', len(body)+1, kind)+body)
            return exact(sock, struct.unpack('>I', exact(sock, 4))[0])

    try:
        cluster.form()
        helper = ticket_put.build_helper(repo, Path(work)/'client-put-ticket')
        ino = ticket_put.create(helper, cluster.base, 'tail', env=cluster.env)
        rc, ticket = ticket_put.admit(helper, cluster.base, ino, Path(work)/'ticket.bin', env=cluster.env)
        assert rc == 0, ('admission refused', rc)
        node = ticket.nodes[0]-1
        assert rpc(node, 6, ticket_put.unticketed_body(ino))[:2] == bytes([7, 1]), 'unticketed PUT accepted'
        assert rpc(node, 6, ticket.body(0))[:2] == bytes([7, 0])
        checksum = bytes.fromhex('70ff942c316810ac5ffe7081fc049dba30713646b1d6d7272cd5948ad5579804')
        get = Get(eid=1, ino=ino, gen=ticket.object)
        fragment = next((cluster.roots(node)[0]/'data/exports/1').rglob(f'0.0.{ticket.object}'))
        original = fragment.read_bytes()
        assert len(original) == 65536+4096

        def check(label, accepted):
            reply = rpc(node, 4, bytes(get))
            if accepted:
                assert reply[:2] == bytes([5, 0]) and reply[2:34] == checksum and reply[34:] == b'x'*65536, (label, reply[:40])
            else:
                # I25: a refused record is INTEGRITY (3), never served.
                assert reply == bytes([5, 3]), (label, reply[:40])
            print(label, 'PASS', flush=True)

        check('direct padded tail', True)
        for length in (128, 4096):
            with fragment.open('r+b') as f:
                f.write(original[:65536+length])
                f.truncate(65536+length)
                f.flush()
                os.fsync(f.fileno())
            check('stored tail length %d' % length, True)
        for length in (127, 0):
            with fragment.open('r+b') as f:
                f.truncate(65536+length)
                f.flush()
                os.fsync(f.fileno())
            check('incomplete tail length %d refused' % length, False)
        for corruption in ('payload', 'record'):
            data = bytearray(original[:65536+128])
            data[0 if corruption == 'payload' else 65536] ^= 1
            with fragment.open('wb') as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            check('corrupt %s refused' % corruption, False)
    finally:
        cluster.close()
print('direct GET stored-tail smoke PASS')
