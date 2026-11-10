#!/usr/bin/env python3
"""K109: a clean unmount while storage members are alive but stalled.

Usage: python3 tests/live/k109_stalled_member_unmount.py [--runs N] [--refused M]
           [--report-only] [--port-base N] [--keep]

Run from the root of a built tree (make, make tests/faults/efsd). Three private
fault-build efsd on 127.0.0.1. The group leaders sit on node 1. A stalled
follower runs with its compactor parked (EFS_FAULT_COMPACT_STALL + a marker
file) and a small L0/memtable budget (EFS_FAULT_L0_BYTES, EFS_FAULT_MEMTABLE_BYTES,
the K22 rehearsal caps): metadata load fills its L0, its apply defers and it
stays alive and reachable but cannot read the session shard (the state of Task
EZ's batch S2). The load then stops.

  phase 1: node 3 stalled (or, with --sigstop, SIGSTOPped: alive, accepting, never answering; node 1 is
           then stopped with a fence worker still parked, and must exit 0). N times a client mounts on node 1, writes and fsyncs
           a few files and unmounts with `scripts/client.sh stop`. Each must exit
           0 with `session revoked at unmount ... rc=0` in with ms= under CLEAN_MS.
  phase 2: node 2 stalled as well (two of three members). M times the same
           unmount must fail closed at phase=storage-fence (no strict majority sealed).

One line per run. --report-only prints the lines and never fails (the
reproduction on an unfixed tree).
"""
import argparse, os, re, shutil, signal, subprocess, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from k33_daemons import Cluster, Mount, mgmt, payload, place_leaders  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
L0_BYTES, MEMTABLE_BYTES = str(1 << 20), str(256 << 10)
CLEAN_MS = 5000   # the old failure took 15000 ms; a clean unmount with a healthy majority takes about 300 ms


def kv(text):
    return {k: int(v) for k, v in re.findall(r'(kv_\w+)=(\d+)', text)}


def stalled(c, i):
    rc, out = mgmt(c.env, 'raft-status', c.ep(i))
    v = kv(out)
    return rc == 0 and v.get('kv_l0_cap', 0) > 0 and v.get('kv_l0_bytes', 0) >= v['kv_l0_cap'] and \
        v.get('kv_apply_deferred', 0) > 0


def fill(c, victims, deadline):
    """Metadata load through node 1 until every victim's L0 is full and its apply deferred."""
    stop = threading.Event()
    procs = []

    def loop(w):
        r = 0
        while not stop.is_set():
            r += 1
            p = subprocess.Popen([str(ROOT / 'efs-bench'), c.ep(1), '--meta', '--files', '2000', '--dirs', '16',
                                  '--workers', '8', '--phases', 'mkdir,create', '--keep',
                                  '--id', str(w * 1000000 + r)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            procs.append(p)
            p.wait()

    threads = [threading.Thread(target=loop, args=(w,), daemon=True) for w in range(2)]
    for t in threads:
        t.start()
    end, t0 = time.time() + deadline, time.time()
    try:
        while time.time() < end:
            time.sleep(2)
            if all(stalled(c, v) for v in victims):
                print(f'stall reached on node(s) {victims} after {time.time() - t0:.0f} s', flush=True)
                return
        raise AssertionError(f'node(s) {victims} never reached the L0 bound within {deadline} s')
    finally:
        stop.set()
        for p in procs:
            if p.poll() is None:
                p.terminate()
        for t in threads:
            t.join(timeout=30)


def one_run(c, run, files, node=1, freeze=None):
    name = f'm{run}'
    m = Mount(c, node, name)
    for k in range(files):
        fd = os.open(m.path / f'{name}-f{k}', os.O_WRONLY | os.O_CREAT, 0o644)
        os.write(fd, payload(f'{name}-f{k}', 65536 + k * 4099))
        os.fsync(fd)
        os.close(fd)
    if freeze:
        freeze.send_signal(signal.SIGSTOP)   # alive and accepting, never answers
    t0 = time.time()
    st = subprocess.run([str(ROOT / 'scripts/client.sh'), 'stop', str(m.path)],
                        capture_output=True, text=True, timeout=300)
    secs = time.time() - t0
    try:
        rc = m.p.wait(timeout=120)
    except subprocess.TimeoutExpired:
        rc = None
    m.log.close()
    rev = [l for l in m.logp.read_text(errors='replace').splitlines() if 'session revoked at unmount' in l]
    return st.returncode, rc, secs, rev[-1] if rev else ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--runs', type=int, default=20)
    ap.add_argument('--refused', type=int, default=0)
    ap.add_argument('--files', type=int, default=4)
    ap.add_argument('--report-only', action='store_true')
    ap.add_argument('--sigstop', action='store_true', help='phase 1 stalls node 3 with SIGSTOP instead of the parked compactor')
    ap.add_argument('--client-node', type=int, default=1, help='node the client mounts (2 with --split)')
    ap.add_argument('--split', action='store_true', help='group 0 leader on node 1, group 2 leader on node 2')
    ap.add_argument('--fill-deadline', type=int, default=900)
    ap.add_argument('--port-base', type=int, default=0, help='fixed ports PORT_BASE+1.. (default: free ports)')
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    work = Path(tempfile.mkdtemp(prefix='efs-k109-', dir=os.environ.get('TMPDIR')))
    env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_TEST_')}
    env['EFS_TRANSPORT'] = 'tcp'
    ids = [1, 2, 3]
    c = Cluster(work, 'c', ids, env, disc_port=a.port_base + 99 if a.port_base else 0)
    if a.port_base:
        for h in c.held:
            h.close()
        c.held = []
        c.ports = {i: a.port_base + i for i in ids}
    bad = total = 0

    def phase(n, victims, want_clean):
        nonlocal bad, total
        for run in range(1, n + 1):
            freeze = c.procs[3] if a.sigstop and want_clean else None
            stop_rc, rc, secs, line = one_run(c, f'{len(victims)}-{run}', a.files, a.client_node, freeze)
            if freeze and run < n:
                freeze.send_signal(signal.SIGCONT)
            clean = stop_rc == 0 and rc == 0 and re.search(r'\brc=0\b', line) is not None
            ms = int((re.search(r'\bms=(\d+)', line) or [0, 10**9])[1])
            ok = a.report_only or ((clean and ms < CLEAN_MS) if want_clean else
                                   (not clean and 'phase=storage-fence' in line))
            bad += not ok
            total += 1
            print(f'stalled={len(victims)} run={run} stop_rc={stop_rc} exit={rc} unmount_s={secs:.1f} clean={int(clean)} '
                  f'ok={int(ok)}\n  {line}', flush=True)

    try:
        c.start(1)
        for i in (2, 3):
            c.start(i, join=1, extra_env=dict(EFS_FAULT_COMPACT_STALL='1', EFS_FAULT_COMPACT_FILE=str(work / f'fault-{i}'),
                                              EFS_FAULT_L0_BYTES=L0_BYTES, EFS_FAULT_MEMTABLE_BYTES=MEMTABLE_BYTES))
        for i in ids:
            c.wait_log(i, r'raft-host: formed: 3 members', timeout=120)
        if a.split:
            place_leaders(c, 1, groups=(0,))
            place_leaders(c, 2, groups=(2,))
        else:
            place_leaders(c, 1)
        if not a.sigstop:
            (work / 'fault-3').touch()
            fill(c, [3], a.fill_deadline)
        phase(a.runs, [3], True)
        if not a.sigstop:   # the leader asked node 3 and was told it did not seal
            logs = ''.join(c.log(i) for i in (1, 2))
            assert re.search(r'storage-fence: member=3 floor=\d+ check=0 rc=-', logs), 'node 3 never answered a fence with an error'
        if a.sigstop:
            t0 = time.time()
            c.procs[1].terminate()
            c.expect_exit(1)
            rc1 = c.procs[1].wait(timeout=60)
            print(f'node 1 stopped with a fence worker parked on the stopped node 3: exit {rc1} in {time.time() - t0:.1f} s', flush=True)
            bad += rc1 != 0
            total += 1
            c.procs[3].send_signal(signal.SIGCONT)
        if a.refused:
            (work / 'fault-2').touch()
            fill(c, [2, 3], a.fill_deadline)
            phase(a.refused, [2, 3], False)
    finally:
        if a.sigstop and 3 in c.procs:
            c.procs[3].send_signal(signal.SIGCONT)
        c.stop_all()
        print(f'{"PASS" if not bad else "FAIL"} {total - bad}/{total}', flush=True)
        if a.keep or bad:
            print(f'kept {work}', flush=True)
        else:
            shutil.rmtree(work, ignore_errors=True)
    return 1 if bad or not total else 0


if __name__ == '__main__':
    sys.exit(main())
