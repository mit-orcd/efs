#!/usr/bin/env python3
"""W158 private four-member check of gc_reclamation.py's late-PUT steps.

Runs the same ticket calls as the expiry fixture's late-PUT control and its
tail fence (tests/live/ticket_put.py, EFS_PROBE_NODES=4) on a disposable
formed four-member cluster, without the rest of the fixture:
  * a ticket admitted on a live inode is accepted by every placed member;
  * an unticketed PUT is refused (status 1) by every member, nothing stored;
  * after unlink, reclaim and a restart of every member, the ticket held
    from the live inode is refused by every placed member, a fresh
    admission is refused, and an unticketed PUT is still refused.
Usage: ticket_put_cluster.py --work DIR [--port 18900] [--source TREE]
Only processes it started are signalled; raw logs stay in --work.
"""
import argparse, os, socket, struct, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ticket_put

ap = argparse.ArgumentParser()
ap.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
ap.add_argument('--work', type=Path, required=True)
ap.add_argument('--port', type=int, default=18900)
a = ap.parse_args()
work = a.work.resolve(); work.mkdir(parents=True, exist_ok=True)
assert 1024 < a.port and a.port + 3 < 65536 and not 17000 <= a.port < 18000, 'never bind 17xxx (deployed clusters)'
HELLO=struct.pack('>IB',17,165)+b'EFSP'+struct.pack('=IQ',5,0)  # CLIENT_HELLO, EFS_PROTOCOL_VERSION 5 (K13)

def exact(sock, n):
    b = b''
    while len(b) < n:
        part = sock.recv(n - len(b)); assert part, 'short reply'; b += part
    return b

def declare(sock):
    sock.sendall(HELLO);n=struct.unpack('>I',exact(sock,4))[0];reply=exact(sock,n)
    assert n==9 and reply[0]==166 and struct.unpack('=iI',reply[1:])==(0,5),('declaration refused',reply)

cluster = ticket_put.Cluster(a.source, work, members=4, base=a.port, extra=['--quota', '1G'], env=os.environ)

def put_reply(node, body):
    with socket.create_connection(('127.0.0.1', cluster.base + node), timeout=5) as sock:
        declare(sock);sock.sendall(struct.pack('>IB',len(body)+1,ticket_put.PUT_CHUNK)+body)
        return exact(sock, struct.unpack('>I', exact(sock, 4))[0])

def ticketed_put(ticket, fi):
    deadline = time.monotonic() + 15
    while True:
        reply = put_reply(ticket.nodes[fi]-1, ticket.body(fi))
        assert len(reply) == 11 and reply[0] == 7, (fi, reply)
        if reply[1] != ticket_put.BUSY or time.monotonic() > deadline: return reply[1]
        time.sleep(.1)

def stored():
    return sorted(p for i in range(4) for p in (work / f'n{i+1}').glob('data/exports/1/**/*') if p.is_file())

def unticketed_refused(ino):
    before = stored()
    for node in range(4):
        reply = put_reply(node, ticket_put.unticketed_body(ino))
        assert len(reply) == 11 and reply[:2] == bytes([7, ticket_put.ERROR]), ('unticketed PUT not refused', node, reply)
    assert stored() == before, 'a refused PUT stored a fragment'

kw = dict(members=4, env=cluster.env)
try:
    for p in range(a.port, a.port + 4):
        with socket.socket() as s: s.bind(('127.0.0.1', p))
    cluster.form()
    helper = ticket_put.build_helper(a.source, work / 'client-put-ticket')
    ino = ticket_put.create(helper, cluster.base, 'fault', **kw)
    rc, late = ticket_put.admit(helper, cluster.base, ino, work / 'late.bin', **kw)
    assert rc == 0, rc
    statuses = [ticketed_put(late, fi) for fi in range(3)]
    assert statuses == [ticket_put.OK] * 3, statuses
    assert len(stored()) == 3, stored()
    print(f'valid ticketed late-PUT control (object {late.object}) accepted on live inode by every placed member {late.nodes} PASS', flush=True)
    unticketed_refused(ino)
    print('unticketed PUT refused (status 1) by every member, no fragment stored (K13) PASS', flush=True)
    held_ino = ticket_put.create(helper, cluster.base, 'offline-member', **kw)
    rc, held = ticket_put.admit(helper, cluster.base, held_ino, work / 'held.bin', **kw)
    assert rc == 0, rc
    ticket_put.unlink(helper, cluster.base, 'fault', **kw)
    ticket_put.unlink(helper, cluster.base, 'offline-member', **kw)
    deadline = time.monotonic() + 60
    while stored():
        assert time.monotonic() < deadline, ('fragments not reclaimed', stored())
        time.sleep(.2)
    print('unlink reclaims the uploaded, never-reported ticketed fragments on every member PASS', flush=True)
    for i in range(4): assert cluster.stop(i) == 0
    for i in range(4): cluster.start(i)
    time.sleep(3)
    statuses = [ticketed_put(held, fi) for fi in range(3)]
    assert all(s not in (ticket_put.OK, ticket_put.BUSY) for s in statuses), ('late ticketed PUT on a retired inode', statuses)
    rc, fresh = ticket_put.admit(helper, cluster.base, held_ino, work / 'retired.bin', **kw)
    assert rc != 0 and fresh is None, ('ticket admitted on a retired inode', rc)
    unticketed_refused(held_ino)
    assert not stored(), stored()
    print(f'after restart: late PUT fenced on every member (held ticket statuses {statuses}, fresh admission rc={rc}, unticketed refused) PASS', flush=True)
    print('W158 four-member late-PUT check PASS', flush=True)
finally:
    cluster.close()
