#!/usr/bin/env python3
"""K15/K29 accounting rule of tests/live/append_failover.py (no cluster).

A write the client saw fail, whose record is absent and whose zero hole (if
any) a matching abandon explains, is accounted, not a mismatch: a load stall
with no leader change and no data loss must not FAIL integrity. An
acknowledged record that is lost, a failed write that landed, or a hole that
no abandon explains stay mismatches. Synthetic files and client-log lines,
positive cases and negative controls.
"""
import sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'live'))
import append_failover as af  # noqa: E402

HOLE = bytes(af.REC)
tmp = Path(tempfile.mkdtemp(prefix='efs-k15acct-'))


def judge(blocks, acked, failed, log_mnt='', log_peer=''):
    path = tmp / 'f'
    path.write_bytes(b''.join(blocks))
    lost, abandons = [], []
    for mount, text in (('mnt', log_mnt), ('peer', log_peer)):
        l, ab = af.client_events(text, mount)
        lost += l
        abandons += ab
    acct = af.account(failed, lost, abandons)
    ok, view = af.verify(path, acked, failed, acct['holes_min'], acct['holes_max'])
    return ok and acct['ok'], acct, view


def lost_line(seq):
    return f'append-reserve fault ino=4097 seq={seq} lost-reply=1\n'


def abandon_line(seq, rc, retired):
    return f'2026-10-23T03:46:41.000Z append-abandon ino=4097 seq={seq} len=4096 rc={rc} retired={retired}\n'


R = af.body
# Writer 0 acknowledged 0..5; seq 2 and 4 failed. Writer 1 acknowledged 0..3; seq 1 failed.
acked = {0: [0, 1, 3, 5], 1: [0, 2, 3]}
failed = {0: [2, 4], 1: [1]}
base = [R(0, 0), R(1, 0), R(0, 1), R(0, 3), R(1, 2), R(0, 5), R(1, 3)]

# 1. Load stall, no injection: the three failures are real timeouts. One
# abandon retired its reservation (one hole), one retired nothing, one write
# failed before reserving (EBUSY, no abandon line). Accounted, PASS.
log = abandon_line(11, 0, 1) + abandon_line(12, 0, 0)
ok, acct, view = judge(base[:3] + [HOLE] + base[3:], acked, failed, log)
assert ok, (acct, view)
assert acct['accounted_failures'] == 3 and acct['injected_losses'] == 0, acct
assert acct['accounted_abandons']['retired'] == 1 and acct['accounted_without_abandon'] == 1, acct
assert (acct['holes_min'], acct['holes_max']) == (1, 2) and view['holes'] == 1, (acct, view)
# The abandon that retired nothing may still leave a hole (cleanup resolved it).
ok, acct, view = judge(base[:3] + [HOLE] + base[3:5] + [HOLE] + base[5:], acked, failed, log)
assert ok and view['holes'] == 2, (acct, view)

# 2. The intel2 lost-1 shape: injected losses plus real timeouts, one BUSY
# abandon (rc=-13) that did not complete. Injected: one hole each.
log = (lost_line(101) + abandon_line(101, 0, 1) + lost_line(198) + abandon_line(198, 0, 1) +
       abandon_line(1271, -13, 0))
ok, acct, view = judge([HOLE] + base[:4] + [HOLE] + base[4:], acked, failed, log)
assert ok, (acct, view)
assert acct['injected_losses'] == 2 and acct['accounted_failures'] == 1, acct
assert acct['accounted_abandons']['not_completed'] == 1 and (acct['holes_min'], acct['holes_max']) == (2, 3), acct
# The BUSY abandon's reservation later resolved as a hole by cleanup: still PASS.
ok, acct, view = judge([HOLE] + base[:4] + [HOLE, HOLE] + base[4:], acked, failed, log)
assert ok and view['holes'] == 3, (acct, view)
# A retry of the same identity logs twice; the last line decides.
log2 = log + abandon_line(1271, 0, 1)
ok, acct, view = judge([HOLE] + base[:4] + [HOLE, HOLE] + base[4:], acked, failed, log2)
assert ok and (acct['holes_min'], acct['holes_max']) == (3, 3), (acct, view)
# Abandons on the peer mount (a second session) count the same way.
ok, acct, view = judge(base[:3] + [HOLE] + base[3:], acked, failed, '', abandon_line(5, 0, 1))
assert ok, (acct, view)

# Negative controls: each must be a mismatch.
log = abandon_line(11, 0, 1) + abandon_line(12, 0, 0)
# 3. An acknowledged record lost (overwritten by a zero hole): the extra hole
# exceeds what the abandons explain.
blocks = base[:3] + [HOLE] + base[3:]
blocks[1] = HOLE
ok, acct, view = judge(blocks, acked, failed, log)
assert not ok and view['missing'] == 1, (acct, view)
# 4. An acknowledged record lost with no hole.
ok, acct, view = judge(base[:2] + [HOLE] + base[3:], acked, failed, log)
assert not ok and view['missing'] == 1, (acct, view)
# 5. A hole without any abandon (no abandon lines at all).
ok, acct, view = judge(base[:3] + [HOLE] + base[3:], acked, failed, '')
assert not ok and view['holes'] == 1 and acct['holes_max'] == 0, (acct, view)
# 6. A retiring abandon whose hole vanished (fewer holes than retirements).
ok, acct, view = judge(base, acked, failed, log)
assert not ok and acct['holes_min'] == 1 and view['holes'] == 0, (acct, view)
# 7. A failed write's record visible.
ok, acct, view = judge(base[:3] + [HOLE, R(0, 2)] + base[3:], acked, failed, log)
assert not ok and view['unacknowledged_visible'] == 1, (acct, view)
# 8. An injected loss whose abandon did not succeed.
log = lost_line(101) + abandon_line(101, -13, 0) + abandon_line(11, 0, 1)
ok, acct, view = judge([HOLE] + base[:3] + [HOLE] + base[3:], acked, failed, log)
assert not ok and acct['mismatches'], (acct, view)
# 9. An acknowledged record duplicated.
ok, acct, view = judge(base[:3] + [HOLE] + base[3:] + [R(1, 3)], acked, failed, abandon_line(11, 0, 1))
assert not ok and view['duplicates'] == 1, (acct, view)
# 10. More abandoned identities than failed writes.
log = ''.join(abandon_line(20 + i, 0, 0) for i in range(4))
ok, acct, view = judge(base, acked, failed, log)
assert not ok and acct['mismatches'], (acct, view)
# 11. Per-writer order broken.
blocks = base[:3] + [HOLE] + base[3:]
blocks[0], blocks[2] = blocks[2], blocks[0]
ok, acct, view = judge(blocks, acked, failed, abandon_line(11, 0, 1))
assert not ok and view['order_errors'] >= 1, (acct, view)

for f in tmp.iterdir():
    f.unlink()
tmp.rmdir()
print('append failover accounting (K15/K29): accounted failures separated from mismatches, '
      '9 negative controls refused: pass')

# Availability is separate: explained real errors remain failed gates.
assert not af.availability(dict(accounted_failures=1,failed_writes=1),[dict(error_count=1)])
assert af.availability(dict(accounted_failures=0,failed_writes=4),[dict(error_count=4)])
assert not af.availability(dict(accounted_failures=0,failed_writes=0),[dict(error_count=1)])
assert af.availability(dict(accounted_failures=0,failed_writes=0),[dict(error_count=0)])
print('append availability: real write/fsync errors fail even when integrity is accounted: pass')

# Formation readiness follows exactly one configured live hint. An exit-zero
# redirect is never treated as a root, and bogus hints are never contacted.
from types import SimpleNamespace
class Formation:
    def __init__(self,replies):self.replies=list(replies);self.calls=[]
    def addr(self,n):return f'node{n}'
    def mgmt(self,*args):
        self.calls.append(args)
        return SimpleNamespace(returncode=0,stdout=self.replies.pop(0))
c=Formation(['raft-getattr status=7 primary=2 ino=0','raft-getattr status=0 primary=2 ino=1'])
assert 'status=0' in af.formation_root(c).stdout
assert c.calls==[('raft-getattr','node1',1),('raft-getattr','node2',1)]
c=Formation(['raft-getattr status=0 primary=1 ino=1'])
assert 'status=0' in af.formation_root(c).stdout and len(c.calls)==1
c=Formation(['raft-getattr status=7 primary=99 ino=0'])
assert 'status=7' in af.formation_root(c).stdout and len(c.calls)==1
c=Formation(['raft-getattr status=7 primary=2 ino=0','raft-getattr status=7 primary=3 ino=0'])
assert 'status=7' in af.formation_root(c).stdout and len(c.calls)==2
print('append formation: one live-hint hop, no false-ready redirect or invalid peer: pass')
