#!/usr/bin/env python3
"""Private daemon RPC check: store read and reclaim removal counters.

K13 (W158): a fragment is stored only under an ADMITTED ticket, so the
check runs a disposable formed three-member cluster (tests/live/ticket_put.py),
admits one object through the production client ledger and uploads its
fragment 0 to its member with the ticket header. The single-fragment
GC_FRAGMENT request (99) is retired; removal now runs through file unlink
and node reclaim (W124).
"""
import argparse
import ctypes as c
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mode',choices=('direct','buffered'),default='buffered')
args=parser.parse_args()
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root/'tests/live'))
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

with tempfile.TemporaryDirectory(prefix='efs-io-stats-') as work:
    cluster = ticket_put.Cluster(root, work, extra=['--quota', '16M',
        '--direct-io' if args.mode=='direct' else '--no-direct-io'], env=os.environ)

    def rpc(node, kind, body):
        with socket.create_connection(('127.0.0.1', cluster.base+node), 5) as sock:
            declare(sock);sock.sendall(struct.pack('>IB', len(body)+1, kind)+body)
            return exact(sock, struct.unpack('>I', exact(sock, 4))[0])

    def status(command, node):
        return subprocess.check_output([str(root/'efs-mgmt'), command,
                                        f'127.0.0.1:{cluster.base+node}'], text=True)
    try:
        cluster.form()
        helper = ticket_put.build_helper(root, Path(work)/'client-put-ticket')
        ino = ticket_put.create(helper, cluster.base, 'io-stats', env=cluster.env)
        rc, ticket = ticket_put.admit(helper, cluster.base, ino, Path(work)/'ticket.bin', env=cluster.env)
        assert rc == 0, ('admission refused', rc)
        node = ticket.nodes[0]-1
        assert rpc(node, 6, ticket_put.unticketed_body(ino))[:2] == bytes([7,1]), 'unticketed PUT accepted'
        assert rpc(node, 6, ticket.body(0))[:2] == bytes([7,0])
        get=Get(eid=1,ino=ino,gen=ticket.object)
        reply=rpc(node,4,bytes(get))
        assert reply[:2] == bytes([5,0]) and reply[34:] == b'x'*65536, (reply[:40],len(reply))
        ticket_put.unlink(helper, cluster.base, 'io-stats', env=cluster.env)
        deadline = time.monotonic()+60
        while 'removed_fragments=1 ' not in status('gc-status', node):
            assert time.monotonic() < deadline, status('gc-status', node)
            time.sleep(.2)
        assert rpc(node,4,bytes(get))[:2] == bytes([5,1])
        io=status('io-stats', node); gc=status('gc-status', node)
        rows={}
        for line in io.splitlines()[1:]:
            rows[line.split()[0]]={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)}
        assert rows['disk_read']['ops']==2 and rows['disk_read']['bytes']==65536
        assert rows['disk_read']['errors']==1 and rows['disk_write']['ops']==1
        assert rows['put_chunk']['ops']==2 and rows['put_chunk']['errors']==1
        assert 'removed_fragments=1 ' in gc and 'reclaimed_payload_bytes=65536 ' in gc
        assert re.search(r'gc_io=unlink ops=1 ', gc), gc
        print(io+gc)
        print('I/O stats: ticketed PUT/GET, refused unticketed PUT, missing read after unlink and single reclaim removal PASS')
    finally:
        cluster.close()
