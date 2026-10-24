#!/usr/bin/env python3
"""Private Linux RPC corruption gate for recorded fragment integrity evidence.

K13 (W158): every stored fragment names an ADMITTED ticket, so each
configuration runs a disposable formed three-member cluster
(tests/live/ticket_put.py), admits one object through the production client
ledger, and drives fragment 0 on its member with the ticket header. An
unticketed PUT, a body of another length, other bytes and a tampered ticket
are refused (status 1). The ticket header has no path hint: every retry
probes the member's roots.
"""
import ctypes as c
import os
import re
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

source = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(source/'tests/live'))
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

for direct in (True,):  # D42: O_DIRECT only
    for roots in (1, 2):
        with tempfile.TemporaryDirectory(prefix='efs-integrity-') as work:
            cluster=ticket_put.Cluster(source,work,extra=['--quota','16M'],
                                       storage=lambda i:[Path(work)/f'n{i+1}'/f'root{index}' for index in range(roots)],
                                       env=os.environ)
            try:
                cluster.form()
                helper=ticket_put.build_helper(source,Path(work)/'client-put-ticket')
                ino=ticket_put.create(helper,cluster.base,'integrity',env=cluster.env)
                rc,ticket=ticket_put.admit(helper,cluster.base,ino,Path(work)/'ticket.bin',env=cluster.env)
                assert rc==0,('admission refused',rc)
                node=ticket.nodes[0]-1
                endpoint=f'127.0.0.1:{cluster.base+node}'
                def rpc(kind, body):
                    with socket.create_connection(('127.0.0.1', cluster.base+node), 5) as sock:
                        declare(sock);sock.sendall(struct.pack('>IB',len(body)+1,kind)+body)
                        return exact(sock,struct.unpack('>I',exact(sock,4))[0])
                def put_ok():
                    # BUSY (leadership/pressure, e.g. after a restart) is the
                    # only retryable verdict, with the same ticket and bytes.
                    for _ in range(150):
                        reply=rpc(6,ticket.body(0))
                        if reply[:2]!=bytes([7,ticket_put.BUSY]):break
                        time.sleep(.1)
                    assert reply[:2]==bytes([7,0]),reply
                def stored():
                    return [path for root in cluster.roots(node) for path in (root/'data/exports/1').rglob('*') if path.is_file()]
                assert rpc(6,ticket_put.unticketed_body(ino))[:2]==bytes([7,1]),'unticketed PUT accepted'
                rejected=rpc(6,ticket.body(0,data=b'z'*131072))
                assert rejected[:2]==bytes([7,1]),rejected
                assert rpc(6,ticket.body(0,data=b'z'*64))[:2]==bytes([7,1])
                assert not stored(),stored()
                # Rejected traffic cannot teach a different fragment size.
                put_ok()
                get=Get(eid=1,ino=ino,gen=ticket.object)
                def good():
                    reply=rpc(4,bytes(get))
                    assert reply[:2]==bytes([5,0]) and reply[34:]==b'x'*65536,reply[:40]
                def bad(label):
                    # I25/K16: a refused fragment record is INTEGRITY (3),
                    # never served.
                    reply=rpc(4,bytes(get))
                    assert reply==bytes([5,3]),(label,reply[:40])
                    print(f'{direct=} {roots=} {label}: refused (INTEGRITY) PASS',flush=True)
                good()
                # Model an ambiguous PUT: send the complete request and
                # discard its reply. Retries carry no root hint and must
                # probe, retaining exactly one object and charge.
                usage_before=subprocess.run([str(source/'efs-mgmt'),'gc-status',endpoint],capture_output=True,text=True,check=True).stdout
                body=ticket.body(0)
                with socket.create_connection(('127.0.0.1',cluster.base+node),5) as lost:
                    declare(lost);lost.sendall(struct.pack('>IB',len(body)+1,6)+body)
                    # Wait for the reply to prove processing, then discard
                    # it without parsing status.
                    assert lost.recv(1,socket.MSG_PEEK)
                for retry in range(20):
                    put_ok()
                usage_after=subprocess.run([str(source/'efs-mgmt'),'gc-status',endpoint],capture_output=True,text=True,check=True).stdout
                assert re.search(r'data_usage_bytes=(\d+)',usage_before).group(1)==re.search(r'data_usage_bytes=(\d+)',usage_after).group(1),(usage_before,usage_after)
                retry_files=stored()
                assert len(retry_files)==1,retry_files
                good()
                print(f'{direct=} {roots=} hintless ticketed PUT retries preserve one object and quota charge PASS',flush=True)
                print(f'{direct=} {roots=} unticketed PUT and rejected length/truncated body refused; fragment size unchanged PASS',flush=True)
                shrink=subprocess.run([str(source/'efs-mgmt'),'shrink-quota',endpoint,'8M'],capture_output=True,text=True)
                assert shrink.returncode==0 and 'Reduced running quota' in shrink.stdout and 'migration' not in shrink.stdout,shrink
                status=subprocess.run([str(source/'efs-mgmt'),'status',endpoint],capture_output=True,text=True,check=True).stdout
                assert 'quota=8.00 MiB' in status,status
                refused=subprocess.run([str(source/'efs-mgmt'),'shrink-quota',endpoint,str(8*1024*1024-32768)],capture_output=True,text=True)
                assert refused.returncode!=0 and 'migration' not in refused.stdout,refused
                status=subprocess.run([str(source/'efs-mgmt'),'status',endpoint],capture_output=True,text=True,check=True).stdout
                assert 'quota=8.00 MiB' in status,status
                good()
                print(f'{direct=} {roots=} running quota reduced; below-usage reduction refused; bytes retained PASS',flush=True)
                # QUERY_STATS (25) must answer UNSUPPORTED (1) in a one-byte
                # QUERY_STATS_REPLY (26), never totals, on a nonempty export.
                assert rpc(25,b'')==bytes([26,1]),'retired QUERY_STATS returned totals'
                print(f'{direct=} {roots=} nonempty export query: unsupported, no false totals PASS',flush=True)
                assert rpc(6,ticket.body(0,data=b'y'*65536))[:2] == bytes([7,1]), 'bytes other than the ticket digest accepted'
                tampered=bytearray(ticket.wire);tampered[-1]^=1
                assert rpc(6,ticket.body(0,wire=bytes(tampered)))[:2] == bytes([7,1]), 'tampered ticket accepted'
                good()  # A rejected retry cannot damage the existing object.
                print(f'{direct=} {roots=} wrong PUT bytes and tampered ticket: rejected, existing bytes retained PASS',flush=True)
                candidates=stored()
                assert len(candidates)==1,candidates
                path=candidates[0];original=path.read_bytes()
                with path.open('r+b') as fragment:fragment.write(b'y')
                bad('payload flip')
                path.write_bytes(original);good()
                assert cluster.stop(node)==0
                cluster.start(node)
                good()
                usage_restart=subprocess.run([str(source/'efs-mgmt'),'gc-status',endpoint],capture_output=True,text=True,check=True).stdout
                assert re.search(r'data_usage_bytes=(\d+)',usage_after).group(1)==re.search(r'data_usage_bytes=(\d+)',usage_restart).group(1),(usage_after,usage_restart)
                put_ok();good()
                print(f'{direct=} {roots=} hintless retry after member restart retains bytes and accounting PASS',flush=True)
                with path.open('r+b') as fragment:fragment.truncate(65536)
                bad('missing record')
                path.write_bytes(original[:65536+127])
                bad('truncated record')
                path.write_bytes(original)
                with path.open('r+b') as fragment:fragment.seek(65536);fragment.write(b'!')
                bad('record flip')
                path.write_bytes(original);good()
            finally:
                cluster.close()
print('Fragment corruption: O_DIRECT, one/two roots PASS',flush=True)
