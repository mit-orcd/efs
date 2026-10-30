#!/usr/bin/env python3
"""K54 (D46 item 6): the session revoke completes with one storage member down,
and that member refuses the revoked epoch when it returns.

Four formed efsd (tests/live/ticket_put.py Cluster) and the production client
ledger (tests/live/client_put_ticket.c) admit a PUT ticket per scenario; the
revoke is `efs-mgmt raft-session fence` (the same ladder a clean unmount runs:
BEGIN, storage fence, FENCE_LOC, FINISH). Late PUTs are hand-built
EFS_MSG_PUT_CHUNK frames of the admitted ticket.

  1. control, no fault: a ticketed PUT is accepted before the revoke, refused
     (STALE) by every member after it;
  2. (i) one member SIGKILLed, revoke exits 0 (base: rc=-13 phase storage-fence),
     member restarted: its PUT of the old epoch is STALE and stores nothing;
  3. (ii) one member stopped cleanly, revoke exits 0, member restarted and
     replayed at once, before any resync wait: STALE;
  4. (iii) two members down (2 of 4: no storage majority): the revoke fails
     closed with rc=-13 and the phase, not clean; one member back (3 of 4) and
     the same `raft-session fence` resumes it to rc=0 (iv); both back: STALE.

About 90 s; make test-long. Usage: test_k54_member_down_fence.py [TREE]
"""
import os, re, signal, socket, struct, subprocess, sys, tempfile, time
from pathlib import Path
root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tests/live'))
import ticket_put  # noqa: E402

HELLO = struct.pack('>IB', 17, 165) + b'EFSP' + struct.pack('=IQ', 5, 0)


def exact(sock, n):
    b = b''
    while len(b) < n:
        part = sock.recv(n - len(b))
        assert part, 'short reply'
        b += part
    return b


def put_reply(port, body):
    with socket.create_connection(('127.0.0.1', port), timeout=10) as sock:
        sock.sendall(HELLO)
        n = struct.unpack('>I', exact(sock, 4))[0]
        assert n == 9 and exact(sock, n)[0] == 166
        sock.sendall(struct.pack('>IB', len(body) + 1, ticket_put.PUT_CHUNK) + body)
        return exact(sock, struct.unpack('>I', exact(sock, 4))[0])


def put_status(cluster, ticket, fi):
    """The PUT reply status of fragment fi on its placed member; BUSY (leader
    still electing after a restart) is retried inside 30 s."""
    end = time.monotonic() + 30
    while True:
        reply = put_reply(cluster.base + ticket.nodes[fi] - 1, ticket.body(fi))
        assert len(reply) == 11 and reply[0] == 7, reply
        if reply[1] != ticket_put.BUSY or time.monotonic() > end:
            return reply[1]
        time.sleep(.1)


def fence(cluster, ticket, via):
    """Exit status and output of `raft-session fence`; a BEGIN that finds no
    metadata leader, or a cleanup that meets the election after the stopped
    member led a group, is resumed by repeating the command (up to 60 s)."""
    for _ in range(60):
        r = subprocess.run([str(root / 'efs-mgmt'), 'raft-session', f'127.0.0.1:{cluster.base + via}',
                            'fence', ticket.wire[:16].hex()], env=cluster.env, capture_output=True,
                           text=True, timeout=120)
        if not re.search(r'phase=(begin|cleanup)', r.stdout + r.stderr):
            break
        time.sleep(1)
    return r.returncode, r.stdout + r.stderr


def stored(work, nodes):
    return sorted(str(p) for i in nodes for p in (work / f'n{i+1}').glob('data/exports/1/**/*') if p.is_file())


def main():
    with tempfile.TemporaryDirectory(prefix='efs-k54-', dir=os.environ.get('TMPDIR')) as d:
        work = Path(d)
        cluster = ticket_put.Cluster(root, work, members=4, extra=['--quota', '1G'], env=os.environ)
        kw = dict(members=4, env=cluster.env)
        try:
            cluster.form()
            helper = ticket_put.build_helper(root, work / 'client-put-ticket')

            def admit(name):
                ino = ticket_put.create(helper, cluster.base, name, **kw)
                rc, t = ticket_put.admit(helper, cluster.base, ino, work / f'{name}.bin', **kw)
                assert rc == 0, rc
                return t

            def stop(i, sig):
                p = cluster.procs.pop(i)
                p.send_signal(sig)
                p.wait(timeout=60)

            def late_all_stale(t, what):
                got = [put_status(cluster, t, fi) for fi in range(3)]
                assert got == [ticket_put.STALE] * 3, (what, got)

            # 1. control
            t = admit('control')
            assert put_status(cluster, t, 0) == ticket_put.OK
            rc, out = fence(cluster, t, 1)
            assert rc == 0, out
            late_all_stale(t, 'control')
            print('control: PUT accepted before the revoke, STALE on every member after PASS', flush=True)

            # 2. (i) SIGKILLed member
            t = admit('kill')
            victim = next(n - 1 for n in t.nodes if n > 1)  # a placed member, never the seed
            stop(victim, signal.SIGKILL)
            rc, out = fence(cluster, t, 0)
            assert rc == 0, ('revoke with one member SIGKILLed', out)
            cluster.start(victim)
            # The collection of abandoned uploads is what keeps asking a member
            # that missed the seal: it must learn the epoch from the session shard
            # (not stay BUSY) and seal it before it deletes anything. No orphan
            # record exists for this ticket, so the answer is STALE (-14), never BUSY (-13).
            fi = t.nodes.index(victim + 1)
            with socket.create_connection(('127.0.0.1', cluster.base + victim), timeout=30) as sock:
                sock.sendall(HELLO)
                n = struct.unpack('>I', exact(sock, 4))[0]
                exact(sock, n)
                body = struct.pack('>II', 1, fi) + t.wire
                sock.sendall(struct.pack('>IB', len(body) + 1, 177) + body)
                reply = exact(sock, struct.unpack('>I', exact(sock, 4))[0])
            assert reply[0] == 178 and struct.unpack('=i', reply[1:5])[0] == -14, ('orphan delete did not learn the epoch', reply)
            assert (work / f'n{victim+1}/meta/put-session' / t.wire[:16].hex()).is_file(), 'no certificate sealed'
            late_all_stale(t, 'returned after SIGKILL')
            print('(i) member SIGKILLed: revoke rc=0; returned member refuses the old epoch (STALE) PASS', flush=True)

            # 3. (ii) clean stop, replay at once
            t = admit('term')
            victim = next(n - 1 for n in t.nodes if n > 1)
            before = stored(work, [victim])
            stop(victim, signal.SIGTERM)
            rc, out = fence(cluster, t, 0)
            assert rc == 0, ('revoke with one member stopped', out)
            cluster.start(victim)
            late_all_stale(t, 'returned after clean stop, replayed before resync')
            cert = work / f'n{victim+1}/meta/put-session' / t.wire[:16].hex()
            assert cert.is_file(), 'the returned member did not seal the epoch it learned'
            assert stored(work, [victim]) == before, 'the returned member stored a revoked-epoch fragment'
            print('(ii) member stopped cleanly: revoke rc=0; replay before any resync wait is STALE, certificate sealed, nothing stored PASS', flush=True)

            # 4. (iii) two members down, then (iv) resume
            t = admit('two')
            stop(0, signal.SIGKILL)
            stop(3, signal.SIGKILL)
            rc, out = fence(cluster, t, 1)
            assert rc != 0 and 'phase=storage-fence' in out, ('2 of 4 members down must fail closed', rc, out)
            logs = ''.join((work / f'n{i+1}.log').read_text(errors='replace') for i in (1, 2))
            assert 'reached 2 of 4 members, no majority' in logs, 'refused for another reason than the missing storage majority'
            cluster.start(3)
            time.sleep(2)
            rc, out = fence(cluster, t, 1)
            assert rc == 0, ('raft-session fence must resume the failed revoke', out)
            cluster.start(0)
            late_all_stale(t, 'both returned')
            print('(iii) two members down: revoke fails closed at the storage fence; (iv) `raft-session fence` resumes it to rc=0 with 3 of 4; both back: STALE PASS', flush=True)
        finally:
            cluster.close()
    print('K54 member-down revoke: majority-sealed fence, returned member refuses the revoked epoch PASS')


if __name__ == '__main__':
    main()
