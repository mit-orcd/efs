#!/usr/bin/env python3
"""Parallel rm -rf of existing trees on an EFS mount, with unlink latency.

  python3 orcd_mini_bulk_delete.py --threads 16 --out SUMMARY.json DIR [DIR ...]

Each DIR's first-level entries are shared out to THREADS workers, which
remove their subtrees bottom-up (files, then directories), and finally DIR
itself. Prints progress every 30 s to stderr and writes counts, elapsed
time, files/s and unlink p50/p99/max to SUMMARY.json. Exit 0 when every
entry was removed without error.
"""
import argparse, json, os, queue, sys, threading, time

p = argparse.ArgumentParser()
p.add_argument('dirs', nargs='+')
p.add_argument('--threads', type=int, default=16)
p.add_argument('--out', required=True)
a = p.parse_args()

work = queue.Queue()
for d in a.dirs:
    for e in os.scandir(d):
        work.put(e.path)
lock = threading.Lock()
files = dirs = 0
lat = []
errors = []
t0 = time.time()


def rm_tree(top):
    global files, dirs
    if not os.path.isdir(top) or os.path.islink(top):
        paths = [(top, False)]
    else:
        paths = []
        for root, dn, fn in os.walk(top, topdown=False):
            paths += [(os.path.join(root, f), False) for f in fn]
            paths.append((root, True))
    for path, is_dir in paths:
        t = time.perf_counter()
        try:
            (os.rmdir if is_dir else os.unlink)(path)
        except OSError as e:
            with lock:
                if len(errors) < 100:
                    errors.append([path, repr(e)])
            continue
        dt = time.perf_counter() - t
        with lock:
            if is_dir:
                dirs += 1
            else:
                files += 1
                if files % 7 == 0:
                    lat.append(dt)


def worker():
    while True:
        try:
            top = work.get_nowait()
        except queue.Empty:
            return
        rm_tree(top)


th = [threading.Thread(target=worker, daemon=True) for _ in range(a.threads)]
[x.start() for x in th]
last = time.time()
while any(x.is_alive() for x in th):
    time.sleep(1)
    if time.time() - last >= 30:
        last = time.time()
        with lock:
            print('%s files=%d dirs=%d rate=%.0f/s errors=%d' % (time.strftime('%FT%TZ', time.gmtime()), files, dirs,
                  files / (last - t0), len(errors)), file=sys.stderr, flush=True)
for d in a.dirs:
    try:
        os.rmdir(d)
        dirs += 1
    except OSError as e:
        errors.append([d, repr(e)])
el = time.time() - t0
v = sorted(lat)
pct = (lambda q: round(1e3 * v[min(len(v) - 1, int(len(v) * q))], 3)) if v else (lambda q: None)
out = {'dirs_arg': a.dirs, 'files': files, 'dirs': dirs, 'elapsed_s': round(el, 1), 'files_per_s': round(files / el, 1) if el else None,
       'unlink_ms': {'sampled': len(v), 'p50': pct(.5), 'p99': pct(.99), 'max': round(1e3 * v[-1], 3) if v else None},
       'errors': errors}
json.dump(out, open(a.out, 'w'), indent=1)
print(json.dumps({k: out[k] for k in ('files', 'dirs', 'elapsed_s', 'files_per_s', 'unlink_ms')}), flush=True)
sys.exit(0 if not errors else 1)
