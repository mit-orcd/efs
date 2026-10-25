#!/usr/bin/env python3
"""A11 finding 2 (C3/C4, K15 W52): REPORTs and appends across metadata
leader step-downs finish within BOUND_S, with the exact record set.

Three tests/faults/efsd daemons on loopback (tests/k33_daemons.py, free
ports, never 17xxx), one efs-fuse mount on node 1. WRITERS processes append
unique 4 KiB records (tests/live/append_failover.py body()) to one shared
O_APPEND file and fsync every FSYNC_EVERY records, so append reservations,
REPORTs with their W77 units and barrier retirements are always in flight.

1. Once records are visible, EFS_FAULT_DECIDE_LOST is armed on every
   daemon: the next DECIDE COMMIT of a multi-part transaction steps its
   coordinator-group leader down and is lost with the deposed leader's log
   (the caller sees NOT_PRIMARY). Before the fix nothing finished such a
   transaction: its PREPAREs held the inode until the 5 s recovery scan
   (`txn-recover ... age=7.5s -> ABORT`), and every REPORT on the file was
   BUSY until the client's 8 s budget ran out (nuc loopback-3, Oct 24).
2. GAP_S later, STEPDOWNS rounds of operator step-downs (efs-mgmt
   raft-step-down) of both metadata groups' leaders, GAP_S apart, while the
   writers keep going. GAP_S > BOUND_S, so an operation within the bound
   crosses at most one leader change (the bound is per change crossed).

Checks:
- the fault fired, and its coordinator handed the transaction to the
  finisher (`txn-commit ... (finisher abort)`), which decided it
  (`txn-finish coord=C ...`); no daemon logged a recovery-scan resolution
  (`txn-recover ... -> ABORT|COMMIT`) for any transaction;
- every write() and fsync() returned 0 within BOUND_S; the client logged no
  REPORT receive retry (`type=67 ... why=recv`), no exhausted REPORT budget
  (`report-loop`) and no inode RPC that succeeded only after BOUND_S
  (`slow-ok ... us=`);
- the file holds every record exactly once, in per-writer order, no hole,
  size 4 KiB x records.

BOUND_S = 3 s per leader change: three maximum election timeouts
(HOST_ELECT_BASE 100 ticks of 5 ms, randomized to [0.5, 1) s), i.e. one
election, the finisher's ABORT and the client's BUSY backoff (at most
800 ms per retry). EFS_RSD_RECORDS (per writer),
EFS_RSD_KEEP=1 keeps the work directory. Skipped (NOTRUN, exit 0) without
/dev/fuse, fusermount3 or efs-fuse.
"""
import multiprocessing as mp
import os, re, shutil, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / 'live'))
from k33_daemons import Cluster, EFSD, MGMT, Mount, fuse_ok, mgmt  # noqa: E402
from append_failover import REC, body, verify  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('REPORT across step-down: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
WRITERS = 4
PER = int(os.environ.get('EFS_RSD_RECORDS', '400'))
FSYNC_EVERY = 8
STEPDOWNS = 3
BOUND_S = 3.0
GAP_S = 4.0
t0 = time.time()


def writer(index, path, count, q):
    acked, errors, wmax, fmax, slow = [], [], 0.0, 0.0, []
    fd = os.open(path, os.O_WRONLY | os.O_APPEND)
    for seq in range(count):
        b, w0 = time.monotonic(), time.time()
        try:
            if os.write(fd, body(index, seq)) == REC:
                acked.append(seq)
            else:
                errors.append(f'short write seq={seq}')
        except OSError as e:
            errors.append(f'write seq={seq} errno={e.errno}')
        d = time.monotonic() - b
        wmax = max(wmax, d)
        if d > 1.0:
            slow.append(('write', seq, round(d, 2), w0))
        if (seq + 1) % FSYNC_EVERY == 0 or seq + 1 == count:
            b, w0 = time.monotonic(), time.time()
            try:
                os.fsync(fd)
            except OSError as e:
                errors.append(f'fsync seq={seq} errno={e.errno}')
            d = time.monotonic() - b
            fmax = max(fmax, d)
            if d > 1.0:
                slow.append(('fsync', seq, round(d, 2), w0))
    os.close(fd)
    q.put(dict(writer=index, acked=acked, errors=errors, write_max_s=wmax, fsync_max_s=fmax, slow=slow))


def stamp(t):
    return time.strftime('%Y-%m-%dT%H:%M:%S', time.gmtime(t)) + '.%03dZ' % int(t % 1 * 1000)


def slow_report(results, c, mnt):
    """What the slow operations crossed: the writers' operations over 1 s
    with their wall-clock start, then every line the daemons and the
    client logged from one second before the first slow operation began
    to one second after the last one ended (periodic counters left out).
    Printed with a timing failure so one occurrence is attributable (the
    flake sweep of Oct 25 saw 5.7 s writes across a group-2 step-down and
    had only the bound and a rate-limited election log to show for it)."""
    out, lo, hi = [], None, None
    for r in results:
        for kind, seq, d, w0 in r['slow']:
            out.append(f'writer {r["writer"]}: {kind} seq={seq} {d} s at {stamp(w0)}')
            if d >= BOUND_S:
                lo = w0 if lo is None else min(lo, w0)
                hi = w0 + d if hi is None else max(hi, w0 + d)
    if lo is None:
        return '\n'.join(out)
    a, b = stamp(lo - 1.0), stamp(hi + 1.0)
    noise = re.compile(r'kv-obs|iostats|gc-health|packer: segments')
    for name, text in [(f'n{i}', c.log(i)) for i in (1, 2, 3)] + \
            [('client', mnt.logp.read_text(errors='replace'))]:
        lines = [l for l in text.splitlines() if a <= l[:24] <= b and not noise.search(l)]
        out.append(f'--- {name}: {len(lines)} lines in [{a}, {b}]')
        out += [f'{name}: {l[:220]}' for l in lines[:150]]
    return '\n'.join(out)


def leaders(c):
    """{group: logical node} for groups 0 and 2, from each node's raft-status."""
    out = {}
    for i in (1, 2, 3):
        rc, text = mgmt(c.env, 'raft-status', c.ep(i))
        for g, role in re.findall(r'group (\d+) hosted=1 role=(\w+)', text):
            if role.lower().startswith('lead'):
                out[int(g)] = i
    return out


work = Path(tempfile.mkdtemp(prefix='efs-rsd-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items()
       if not k.startswith('EFS_FAULT_') and not k.startswith('EFS_TEST_')}
env['EFS_TRANSPORT'] = 'tcp'
# A failure must be attributable from the logs: every Raft election line
# (not one per 10 s) and the client's per-receive timing.
env['EFS_FAULT_RAFT_ELECT_LOG'] = '1'
env['EFS_RPC_PROF'] = '1'
client_env = dict(env)
fault = work / 'decide-lost'
env['EFS_FAULT_DECIDE_LOST'] = str(fault)
c = Cluster(work, 'r', [1, 2, 3], env)
mnt, procs = None, []
try:
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    c.wait_members([1, 2, 3], 3)
    mnt = Mount(c, 1, 'mnt', env=client_env)
    path = mnt.path / 'append.dat'
    path.write_bytes(b'')
    ctx = mp.get_context('fork')  # the module is a script, not importable by forkserver
    q = ctx.Queue()
    procs = [ctx.Process(target=writer, args=(i, str(path), PER, q)) for i in range(WRITERS)]
    for p in procs:
        p.start()
    end = time.time() + 60
    while time.time() < end and path.stat().st_size < REC * WRITERS * FSYNC_EVERY:
        time.sleep(0.1)
    assert path.stat().st_size >= REC * WRITERS * FSYNC_EVERY, 'appends never became visible'
    fault.write_text('armed\n')
    ready = Path(str(fault) + '.ready')
    end = time.time() + 60
    while time.time() < end and not (ready.exists() and ready.read_text().endswith('\n')):
        time.sleep(0.05)
    assert ready.exists(), 'EFS_FAULT_DECIDE_LOST never fired'
    fired = ready.read_text()
    events = []
    time.sleep(GAP_S)
    for _ in range(STEPDOWNS):
        for g in (0, 2):
            if not any(p.is_alive() for p in procs):
                break
            led = leaders(c)
            if g in led:
                rc, out = mgmt(c.env, 'raft-step-down', c.ep(led[g]), str(g))
                events.append((g, led[g], rc, out.strip()[-120:]))
            time.sleep(GAP_S)
    results = [q.get(timeout=600) for _ in procs]
    for p in procs:
        p.join(timeout=60)
    results.sort(key=lambda r: r['writer'])
    errors = [e for r in results for e in r['errors']]
    assert not errors, f'writer errors: {errors[:10]}'
    wmax = max(r['write_max_s'] for r in results)
    fmax = max(r['fsync_max_s'] for r in results)
    assert wmax <= BOUND_S and fmax <= BOUND_S, \
        f'write max {wmax:.2f} s, fsync max {fmax:.2f} s > {BOUND_S} s\n' + slow_report(results, c, mnt)
    assert sum(1 for e in events if e[2] == 0) >= STEPDOWNS, f'step-downs: {events}'

    # The lost DECIDE went to the finisher, never to the recovery scan.
    coord = re.search(r'coord=(\d+)', fired).group(1)
    logs = {i: c.log(i) for i in (1, 2, 3)}
    owner = [i for i in logs if re.search(rf'decide-lost fault: coord={coord} ', logs[i])]
    assert owner, f'no daemon logged the fault for coord={coord}: {fired}'
    assert re.search(rf'txn-commit coord={coord} parts=\d+ decide rc=-15 \(finisher abort\)',
                     logs[owner[0]]), logs[owner[0]][-3000:]
    assert re.search(rf'txn-finish coord={coord} ', logs[owner[0]]), logs[owner[0]][-3000:]
    recovered = [l for i in logs for l in logs[i].splitlines() if re.search(r'txn-recover .*-> ', l)]
    assert not recovered, f'recovery-scan resolutions: {recovered[:5]}'

    clog = mnt.logp.read_text(errors='replace')
    recv67 = re.findall(r'.*inode-rpc: retry type=67 .*why=recv.*', clog)
    budget = re.findall(r'.*report-loop .*', clog)
    slow = [(int(t), int(us)) for t, us in re.findall(r'slow-ok type=(\d+) .*us=(\d+)', clog)]
    slow_max = max((us for _, us in slow), default=0) / 1e6
    assert not recv67 and not budget, f'REPORT stalls: {(recv67 + budget)[:5]}'
    assert slow_max <= BOUND_S, \
        f'slowest inode RPC {slow_max:.2f} s > {BOUND_S} s: {slow[:10]}\n' + slow_report(results, c, mnt)

    ok, summary = verify(path, {r['writer']: r['acked'] for r in results}, {})
    assert ok, f'record set: {summary}'
    print(f'REPORT across step-down: fault coord={coord} on node {owner[0]} finished by the '
          f'finisher, {len(events)} operator step-downs, {summary["visible_records"]} records exact; '
          f'write max {wmax:.2f} s, fsync max {fmax:.2f} s, slowest RPC {slow_max:.2f} s '
          f'(bound {BOUND_S} s)')
finally:
    for p in procs:
        if p.is_alive():
            p.kill()
    if mnt:
        mnt.close()
    c.stop_all()
    if os.environ.get('EFS_RSD_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print(f'REPORT across metadata step-down within {BOUND_S} s, exact record set: PASS '
      f'({time.time() - t0:.1f} s)')
