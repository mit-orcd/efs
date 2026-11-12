#!/usr/bin/env python3
"""K106 distinct-host case (Task EY's SIGSTOP of one member under a client load), run from the Mac as a qualify plugin.

A mixed load (mkdir, 256 KiB write+fsync+close, stat, readdir; 4 threads) runs on the mount host. After BASE s the
controller SIGSTOPs the efsd that leads metadata group 0 (on its own host), holds it WINDOW s, then SIGCONTs it
and the load runs TAIL s more. Recorded per operation: start, duration, errno. PASS: no operation fails; on TCP also the
first operation started after the stop, and every operation, finished in under 25 s (not the 30 s transport timeout).
On RDMA the wait cut does not cover the connection (K114): the latencies are recorded and a long wait alone is no FAIL."""
import argparse, json, statistics, sys, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gd_lib import ssh, leaders, member_pid, finish, collect

LOAD = r'''
import os, sys, time, json, threading, errno
root, dur, nthreads = sys.argv[1], float(sys.argv[2]), int(sys.argv[3])
os.makedirs(root, exist_ok=True); ops = []; lock = threading.Lock(); end = time.time() + dur
data = os.urandom(256 * 1024)
def rec(t, op, t0, e): 
    with lock: ops.append((t, op, t0, round(time.time() - t0, 4), e))
def run(i):
    d = os.path.join(root, 't%d' % i); os.makedirs(d, exist_ok=True); n = 0
    while time.time() < end:
        n += 1
        t0 = time.time(); e = 0
        try: os.mkdir(os.path.join(d, 'd%d' % n))
        except OSError as x: e = x.errno
        rec(i, 'mkdir', t0, e)
        p = os.path.join(d, 'f%d' % n); fd = -1
        t0 = time.time(); e = 0
        try: fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
        except OSError as x: e = x.errno
        rec(i, 'create', t0, e)
        if fd >= 0:
            t0 = time.time(); e = 0
            try: os.write(fd, data)
            except OSError as x: e = x.errno
            rec(i, 'write', t0, e)
            t0 = time.time(); e = 0
            try: os.fsync(fd)
            except OSError as x: e = x.errno
            rec(i, 'fsync', t0, e)
            t0 = time.time(); e = 0
            try: os.close(fd)
            except OSError as x: e = x.errno
            rec(i, 'close', t0, e)
        t0 = time.time(); e = 0
        try: os.stat(p)
        except OSError as x: e = x.errno
        rec(i, 'stat', t0, e)
        t0 = time.time(); e = 0
        try: os.listdir(d if n % 8 else root)
        except OSError as x: e = x.errno
        rec(i, 'readdir', t0, e)
ts = [threading.Thread(target=run, args=(i,)) for i in range(nthreads)]
[t.start() for t in ts]; [t.join() for t in ts]
for o in ops: print(json.dumps(o))
'''


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--cluster', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--source', required=True); ap.add_argument('--base', type=float, default=15)
    ap.add_argument('--window', type=float, default=60); ap.add_argument('--tail', type=float, default=20)
    a = ap.parse_args(); cl = json.load(open(a.cluster)); out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    m = cl['mounts'][0]; rdma = cl.get('transport') == 'rdma'
    lead = leaders(cl, a.source)
    if 0 not in lead:
        finish(out, 'FAIL', 'no group 0 leader observed', {})
    node = cl['nodes'][lead[0][0]]; pid = member_pid(node)
    dur = a.base + a.window + a.tail + 10
    root = f'{m["path"]}/gd-k106-{int(time.time())}'
    box = {}
    def load():
        box['r'] = ssh(m['host'], f'python3 - {root} {dur} 4', stdin=LOAD, timeout=dur + 300)
    th = threading.Thread(target=load); th.start()
    time.sleep(a.base)
    t_client = ssh(m['host'], 'date +%s.%N')[1].strip()
    rc, so, se = ssh(node['host'], f'date +%s.%N; kill -STOP {pid}; echo stop-rc=$?')
    t_stop = float(so.split()[0]); events = [f'SIGSTOP pid {pid} on {node["host"]} ({node["addr"]}, group 0 leader term {lead[0][2]}): {so.strip()!r} {se.strip()!r}']
    try:
        time.sleep(a.window)
    finally:
        rc, so, se = ssh(node['host'], f'date +%s.%N; kill -CONT {pid}; echo cont-rc=$?')
        events.append(f'SIGCONT: {so.strip()!r} {se.strip()!r}')
    th.join()
    rc, text, err = box['r']
    ops = [json.loads(l) for l in text.splitlines() if l.startswith('[')]
    offset = float(t_client) - t_stop  # client clock minus member clock around the stop (includes the BASE gap error of the ssh calls)
    after = sorted((o for o in ops if o[2] >= t_stop), key=lambda o: o[2])
    first = after[0] if after else None
    spanning = [o for o in ops if o[2] < t_stop <= o[2] + o[3]]
    errs = [o for o in ops if o[4]]
    longest = max(ops, key=lambda o: o[3]) if ops else None
    summary = dict(transport=cl.get('transport'), halted=node['addr'], pid=pid, ops=len(ops), errors=len(errs),
                   write_errors=sum(1 for o in errs if o[1] in ('create', 'write', 'fsync', 'close')),
                   first_after_stop=first, in_flight_at_stop=len(spanning),
                   longest_in_flight_s=max((o[3] for o in spanning), default=0),
                   longest_op=longest, op_p50_s=statistics.median(o[3] for o in ops) if ops else None,
                   clock_probe_s=round(offset, 3), load_rc=rc)
    (out / 'ops.jsonl').write_text(text); (out / 'load.err').write_text(err)
    bad = []
    if rc or not ops:
        bad.append(f'load rc={rc}, {len(ops)} ops')
    if errs:
        bad.append(f'{len(errs)} operation errors: ' + ', '.join(sorted({f"{o[1]}:{o[4]}" for o in errs})))
    if not rdma and ops:
        if first and first[3] >= 25:
            bad.append(f'first operation after the stop took {first[3]} s')
        if longest[3] >= 25:
            bad.append(f'an operation took {longest[3]} s')
    collect(cl, out)
    doc = dict(summary=summary, events=events)
    finish(out, 'FAIL' if bad else 'PASS', '; '.join(bad) or f'no error; first op after stop {first and first[3]} s; longest op {longest and longest[3]} s; in flight at stop {len(spanning)}', doc)


main()
