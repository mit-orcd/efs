#!/usr/bin/env python3
"""W124 mixed delete/read/write workload for one orcd-mini client node.

Run the same command on node9900 and node9901 at the same time (each uses
its own directory under the mount):

  python3 orcd_mini_w124_mixed.py MOUNT --tag n9900 --seconds 900 \
      --prefill 20000 --writers 8 --readers 8 --deleters 8 --out OUTDIR

Phases:
  1. prefill: create PREFILL files (half `keep`, half `doom`), sizes mostly
     tiny (256 B..16 KiB) with a 2% tail of 1..8 MiB files;
  2. mix for SECONDS: writers create new keep/doom files; readers read
     random keep files and check every byte; deleters unlink doom files
     (prefilled ones first, then the writers' doom files);
  3. verify: every keep file reads back exactly, every doom file deleted
     by this run is absent.
Content is a pure function of the file name, so any read can be checked
without a stored copy. Writes are close-to-open: a file enters the keep
list only after close() returned.

Output: OUTDIR/<tag>-summary.json (counts, errors, p50/p99/max latency per
operation, verify verdict) and OUTDIR/<tag>-progress.log (one line per
10 s). Exit status 0 only when there were no errors and verify passed.
Cleanup: rm -rf MOUNT/w124mix-<tag> (the final full-delete phase does this
when --cleanup is given).
"""
import argparse, hashlib, json, os, random, sys, threading, time

p = argparse.ArgumentParser()
p.add_argument('mount')
p.add_argument('--tag', required=True)
p.add_argument('--seconds', type=int, default=900)
p.add_argument('--prefill', type=int, default=20000)
p.add_argument('--writers', type=int, default=8)
p.add_argument('--readers', type=int, default=8)
p.add_argument('--deleters', type=int, default=8)
p.add_argument('--max-new', type=int, default=200000, help='cap on files the writers create')
p.add_argument('--out', required=True)
p.add_argument('--cleanup', action='store_true', help='rm -rf the run directory after verify')
a = p.parse_args()

root = os.path.join(a.mount, 'w124mix-' + a.tag)
os.makedirs(a.out, exist_ok=True)
summary_path = os.path.join(a.out, a.tag + '-summary.json')
progress = open(os.path.join(a.out, a.tag + '-progress.log'), 'a', buffering=1)


def content(name, size):
    seed = hashlib.sha256(name.encode()).digest()
    reps = size // len(seed) + 1
    return (seed * reps)[:size]


def pick_size(rng):
    if rng.random() < 0.02:
        return rng.randint(1, 8) << 20
    return rng.choice((256, 1024, 4096, 8192, 16384))


lock = threading.Lock()
keep, doom = [], []          # (path, size) published after close()
deleted = []                 # doom paths this run unlinked
lat = {k: [] for k in ('create', 'read', 'unlink', 'verify')}
errors = []
counts = {'created': 0, 'read': 0, 'read_bytes': 0, 'unlinked': 0, 'written_bytes': 0}
stop = threading.Event()
seq = [0]


def err(kind, path, e):
    with lock:
        if len(errors) < 200:
            errors.append({'op': kind, 'path': path, 'error': repr(e), 't': time.time()})
        counts['errors_' + kind] = counts.get('errors_' + kind, 0) + 1


def create_one(rng, kind):
    with lock:
        n = seq[0]; seq[0] += 1
    d = os.path.join(root, kind, 'd%04d' % (n // 1000))
    path = os.path.join(d, '%s-%08d' % (kind, n))
    size = pick_size(rng)
    data = content(os.path.basename(path), size)
    t = time.perf_counter()
    try:
        os.makedirs(d, exist_ok=True)
        with open(path, 'xb') as f:
            f.write(data)
    except OSError as e:
        err('create', path, e)
        return
    dt = time.perf_counter() - t
    with lock:
        lat['create'].append(dt)
        counts['created'] += 1
        counts['written_bytes'] += size
        (keep if kind == 'keep' else doom).append((path, size))


def check(path, size, op):
    t = time.perf_counter()
    try:
        with open(path, 'rb') as f:
            got = f.read()
    except OSError as e:
        err(op, path, e)
        return False
    dt = time.perf_counter() - t
    want = content(os.path.basename(path), size)
    with lock:
        lat[op].append(dt)
        if op == 'read':
            counts['read'] += 1
            counts['read_bytes'] += len(got)
    if got != want:
        err(op, path, 'content mismatch: %d bytes, want %d' % (len(got), size))
        return False
    return True


def writer(i):
    rng = random.Random(1000 + i)
    while not stop.is_set():
        with lock:
            if counts['created'] >= a.prefill + a.max_new:
                break
        create_one(rng, 'keep' if rng.random() < 0.5 else 'doom')


def reader(i):
    rng = random.Random(2000 + i)
    while not stop.is_set():
        with lock:
            if not keep:
                item = None
            else:
                item = keep[rng.randrange(len(keep))]
        if item is None:
            time.sleep(0.01); continue
        check(item[0], item[1], 'read')


def deleter(i):
    while not stop.is_set():
        with lock:
            item = doom.pop(0) if doom else None
        if item is None:
            time.sleep(0.01); continue
        t = time.perf_counter()
        try:
            os.unlink(item[0])
        except OSError as e:
            err('unlink', item[0], e)
            continue
        dt = time.perf_counter() - t
        with lock:
            lat['unlink'].append(dt)
            counts['unlinked'] += 1
            deleted.append(item[0])


def pct(v):
    if not v:
        return None
    v = sorted(v)
    return {'n': len(v), 'p50_ms': round(1e3 * v[len(v) // 2], 3),
            'p99_ms': round(1e3 * v[min(len(v) - 1, int(len(v) * 0.99))], 3),
            'max_ms': round(1e3 * v[-1], 3)}


def run(fn, n):
    th = [threading.Thread(target=fn, args=(i,), daemon=True) for i in range(n)]
    [x.start() for x in th]
    return th


start = time.time()
print('%s prefill %d start' % (time.strftime('%FT%TZ', time.gmtime()), a.prefill), file=progress)
rng = random.Random(7)
pre = []
def prefill(i):
    r = random.Random(3000 + i)
    while True:
        with lock:
            if counts['created'] >= a.prefill:
                return
        create_one(r, 'keep' if r.random() < 0.5 else 'doom')
[x.join() for x in run(prefill, max(a.writers, 8))]
prefill_s = time.time() - start
print('%s prefill done %.1fs created=%d' % (time.strftime('%FT%TZ', time.gmtime()), prefill_s, counts['created']), file=progress)

mix_start = time.time()
threads = run(writer, a.writers) + run(reader, a.readers) + run(deleter, a.deleters)
last = 0
while time.time() - mix_start < a.seconds:
    time.sleep(1)
    if time.time() - last >= 10:
        last = time.time()
        with lock:
            print('%s t=%.0f created=%d read=%d unlinked=%d keep=%d doom_pending=%d errors=%d' % (
                time.strftime('%FT%TZ', time.gmtime()), last - mix_start, counts['created'], counts['read'],
                counts['unlinked'], len(keep), len(doom), len(errors)), file=progress)
stop.set()
[x.join() for x in threads]
mix_s = time.time() - mix_start

# Verify: every keep file exact; every deleted doom file absent.
verify_start = time.time()
bad = 0
for path, size in list(keep):
    if not check(path, size, 'verify'):
        bad += 1
present = 0
for path in deleted:
    if os.path.lexists(path):
        present += 1
        err('verify', path, 'deleted file still present')
verify_s = time.time() - verify_start
ok = not errors and bad == 0 and present == 0

cleanup_s = None
if a.cleanup:
    t = time.time()
    for dp, dn, fn in os.walk(root, topdown=False):
        for x in fn:
            os.unlink(os.path.join(dp, x))
        os.rmdir(dp)
    cleanup_s = time.time() - t

out = {'tag': a.tag, 'mount': a.mount, 'root': root, 'verdict': 'PASS' if ok else 'FAIL',
       'seconds': a.seconds, 'prefill': a.prefill, 'threads': {'writers': a.writers, 'readers': a.readers, 'deleters': a.deleters},
       'prefill_s': round(prefill_s, 1), 'mix_s': round(mix_s, 1), 'verify_s': round(verify_s, 1), 'cleanup_s': cleanup_s,
       'counts': counts, 'keep_files': len(keep), 'doom_left': len(doom), 'deleted': len(deleted),
       'verify_bad': bad, 'verify_deleted_present': present,
       'latency': {k: pct(v) for k, v in lat.items()}, 'errors': errors[:50]}
with open(summary_path, 'w') as f:
    json.dump(out, f, indent=1)
print('%s done verdict=%s' % (time.strftime('%FT%TZ', time.gmtime()), out['verdict']), file=progress)
sys.exit(0 if ok else 1)
