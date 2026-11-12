#!/usr/bin/env python3
"""K15 distinct-host append case, run from the Mac as a qualify plugin (W52 20k and the step-down 8k of Task N,
but on a cluster whose members and two mounts sit on different hosts).

WPH writers per mount host (two mounts, so two sessions) each hold one O_APPEND descriptor on one shared file and write
RECORDS/(2*WPH) records of 4096 bytes, then fsync. With --failover step-down the controller steps down the leaders of
both metadata groups (efs-mgmt raft-step-down) every INTERVAL s while they run. Afterwards the file is read through both
mounts. PASS: no write/fsync/close error, no write over BOUND s, every acknowledged record present exactly once and
intact in both views, no zero hole, no foreign bytes."""
import argparse, json, sys, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gd_lib import ssh, leaders, mgmt, finish, collect

WRITE = r'''
import os, sys, time, json, threading, hashlib
path, w0, nw, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
def rec(w, s):
    head = 'K15 W%02d S%06d ' % (w, s)
    head += hashlib.sha256(head.encode()).hexdigest()[:16] + ' '
    return (head + 'x' * (4095 - len(head))).encode() + b'\n'
res = {}
def run(w):
    r = dict(acked=[], errors=[], fsync=None, max_write_s=0.0); res[w] = r
    fd = os.open(path, os.O_WRONLY | os.O_APPEND)
    for s in range(count):
        t0 = time.time()
        try:
            n = os.write(fd, rec(w, s))
            if n != 4096: raise OSError(5, 'short write %d' % n)
            r['acked'].append(s)
        except OSError as e:
            r['errors'].append((s, e.errno))
        r['max_write_s'] = max(r['max_write_s'], time.time() - t0)
    try: os.fsync(fd)
    except OSError as e: r['fsync'] = e.errno
    try: os.close(fd)
    except OSError as e: r['fsync'] = r['fsync'] or ('close', e.errno)
ts = [threading.Thread(target=run, args=(w0 + i,)) for i in range(nw)]
[t.start() for t in ts]; [t.join() for t in ts]
print(json.dumps({str(w): dict(acked=len(r['acked']), acked_seq=r['acked'], errors=r['errors'][:20], nerr=len(r['errors']), fsync=r['fsync'], max_write_s=round(r['max_write_s'], 3)) for w, r in res.items()}))
'''

READ = r'''
import os, sys, json, hashlib, collections
data = open(sys.argv[1], 'rb').read(); seen = collections.defaultdict(collections.Counter); bad = []; holes = 0
for off in range(0, len(data), 4096):
    b = data[off:off + 4096]
    if len(b) < 4096 or b == bytes(4096): holes += 1; continue
    try:
        t = b.decode(); w, s = int(t[5:7]), int(t[9:15]); dig = t[16:32]
        ok = t[:4] == 'K15 ' and t[32] == ' ' and hashlib.sha256(t[:16].encode()).hexdigest()[:16] == dig and set(t[33:-1]) <= {'x'} and t[-1] == '\n'
    except Exception: ok = False
    if not ok: bad.append(off); continue
    seen[w][s] += 1
print(json.dumps(dict(size=len(data), holes=holes, bad=bad[:10], nbad=len(bad), writers={str(w): dict(distinct=len(c), dup=sum(v - 1 for v in c.values()), seqs=sorted(c)) for w, c in seen.items()})))
'''


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--cluster', required=True); ap.add_argument('--out', required=True)
    ap.add_argument('--source', required=True); ap.add_argument('--records', type=int, default=20000)
    ap.add_argument('--wph', type=int, default=2); ap.add_argument('--failover', default='none', choices=['none', 'step-down'])
    ap.add_argument('--interval', type=float, default=4); ap.add_argument('--first-event', type=float, default=3)
    ap.add_argument('--bound', type=float, default=30); a = ap.parse_args()
    cl = json.load(open(a.cluster)); out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    mounts = cl['mounts'][:2]; per = a.records // (len(mounts) * a.wph)
    name = f'gd-k15-{int(time.time())}'
    for m in mounts[:1]:
        rc, _, e = ssh(m['host'], f'touch {m["path"]}/{name}'); assert rc == 0, e
    stop = threading.Event(); events = []; results = {}

    def writer(k, m):
        results[k] = ssh(m['host'], f'python3 - {m["path"]}/{name} {k * a.wph} {a.wph} {per}', stdin=WRITE, timeout=3600)
    ths = [threading.Thread(target=writer, args=(k, m)) for k, m in enumerate(mounts)]
    [t.start() for t in ths]
    def fail_over():
        time.sleep(a.first_event)
        while not stop.is_set():
            for g, (i, addr, term) in sorted(leaders(cl, a.source).items()):
                rc, text = mgmt(cl, a.source, 'raft-step-down', addr, g)
                events.append(dict(t=time.time(), group=g, old=addr, old_term=term, rc=rc, out=text.strip()[-120:]))
            stop.wait(a.interval)
    fo = threading.Thread(target=fail_over) if a.failover == 'step-down' else None
    t0 = time.time()
    if fo: fo.start()
    [t.join() for t in ths]; stop.set()
    if fo: fo.join()
    secs = round(time.time() - t0, 1)
    acked, bad = {}, []
    for k, (rc, text, err) in sorted(results.items()):
        try: r = json.loads(text.strip().splitlines()[-1])
        except Exception: bad.append(f'writer host {k}: rc={rc} output unreadable: {err.strip()[-200:]}'); continue
        for w, v in r.items():
            acked[int(w)] = set(v['acked_seq'])
            if v['nerr'] or v['fsync']: bad.append(f'writer {w}: {v["nerr"]} write errors {v["errors"][:3]}, fsync/close {v["fsync"]}')
            if v['max_write_s'] >= a.bound: bad.append(f'writer {w}: a write took {v["max_write_s"]} s')
    views = {}
    for m in mounts:
        rc, text, err = ssh(m['host'], f'python3 - {m["path"]}/{name}', stdin=READ, timeout=1800)
        try: v = json.loads(text.strip().splitlines()[-1])
        except Exception: bad.append(f'read via {m["host"]} failed rc={rc}: {err.strip()[-200:]}'); continue
        views[m['host']] = dict(size=v['size'], holes=v['holes'], nbad=v['nbad'], dup=sum(x['dup'] for x in v['writers'].values()))
        if v['holes'] or v['nbad']: bad.append(f'{m["host"]} view: {v["holes"]} zero holes/short, {v["nbad"]} bad records')
        for w, s in acked.items():
            got = v['writers'].get(str(w), dict(seqs=[], dup=0))
            have = set(got['seqs']); miss = s - have
            if miss or got['dup']: bad.append(f'{m["host"]} view writer {w}: {len(miss)} acknowledged records missing, {got["dup"]} duplicated')
    ssh(mounts[0]['host'], f'rm -f {mounts[0]["path"]}/{name}')
    collect(cl, out)
    total = sum(len(s) for s in acked.values())
    doc = dict(summary=dict(transport=cl.get('transport'), records=a.records, acknowledged=total, seconds=secs, failover=a.failover,
                            step_downs=len(events), step_down_failed=sum(1 for e in events if e['rc']), views=views), events=events)
    finish(out, 'FAIL' if bad else 'PASS', '; '.join(bad) or f'{total} acknowledged records exact in both views, {len(events)} step-downs, {secs} s', doc)


main()
