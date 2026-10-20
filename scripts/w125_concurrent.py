#!/usr/bin/env python3
"""w125_concurrent.py ROOT SECONDS [--writers 4] [--readers 4] [--deleters 2]

Concurrent writers, readers and deleters on one efs mount, for packing and
compaction under load. Every file's content is a deterministic function of
its name (sizes log-uniform from 1 B to 4 MiB, so most are small and
packed, some are large), so any read can be checked:
  writers   create new files (written, closed, then published to the live set);
  readers   read a random live file and compare it with its expected bytes;
            ENOENT is fine (a deleter won the race), wrong bytes are not;
  deleters  unlink a random live file (kept to about half the files written).
Every 10 s one status line. After SECONDS the workers stop and every
remaining file is verified once more. The final live list is written to
ROOT.live (one name per line). Exit 1 on any mismatch or unexpected error.
"""
import argparse, errno, math, os, random, sys, threading, time

def body(name):
    r = random.Random(name)
    size = int(math.exp(r.uniform(0, math.log(4 << 20))))
    return r.randbytes(max(1, size))

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("root"); ap.add_argument("seconds", type=float)
ap.add_argument("--writers", type=int, default=4); ap.add_argument("--readers", type=int, default=4)
ap.add_argument("--deleters", type=int, default=2)
a = ap.parse_args()
os.makedirs(a.root, exist_ok=True)
lock = threading.Lock()
live, stop = [], threading.Event()
st = dict(written=0, wbytes=0, read=0, rbytes=0, enoent=0, deleted=0, mismatch=0, errors=0)
errs = []
def bump(k, n=1):
    with lock: st[k] += n
def fail(kind, msg):
    with lock:
        st[kind] += 1
        if len(errs) < 20: errs.append("%s %s" % (kind, msg))

def writer(w):
    i = 0
    while not stop.is_set():
        name = "w%d/d%03d/f%06d" % (w, i % 50, i); i += 1
        path = os.path.join(a.root, name)
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            data = body(name)
            with open(path, "wb") as f: f.write(data)
            with lock: live.append(name); st["written"] += 1; st["wbytes"] += len(data)
        except OSError as e:
            fail("errors", "write %s: %s" % (name, e))

def reader(_):
    while not stop.is_set():
        with lock: name = random.choice(live) if live else None
        if not name: time.sleep(0.05); continue
        try:
            with open(os.path.join(a.root, name), "rb") as f: got = f.read()
        except FileNotFoundError:
            bump("enoent"); continue
        except OSError as e:
            fail("errors", "read %s: %s" % (name, e)); continue
        if got != body(name): fail("mismatch", "%s: %d bytes, expected %d" % (name, len(got), len(body(name))))
        else: bump("read"); bump("rbytes", len(got))

def deleter(_):
    while not stop.is_set():
        with lock:
            if len(live) < 20 or st["deleted"] * 2 >= st["written"]: name = None
            else: name = live.pop(random.randrange(len(live)))
        if not name: time.sleep(0.05); continue
        try:
            os.unlink(os.path.join(a.root, name)); bump("deleted")
        except OSError as e:
            fail("errors", "unlink %s: %s" % (name, e))

threads = [threading.Thread(target=writer, args=(i,)) for i in range(a.writers)]
threads += [threading.Thread(target=reader, args=(i,)) for i in range(a.readers)]
threads += [threading.Thread(target=deleter, args=(i,)) for i in range(a.deleters)]
for t in threads: t.start()
t0 = time.time()
while time.time() - t0 < a.seconds:
    time.sleep(10)
    with lock: s = dict(st); n = len(live)
    print("t=%.0fs live=%d written=%d (%.0f MiB) read=%d (%.0f MiB) enoent=%d deleted=%d mismatch=%d errors=%d" % (
        time.time() - t0, n, s["written"], s["wbytes"] / 2**20, s["read"], s["rbytes"] / 2**20,
        s["enoent"], s["deleted"], s["mismatch"], s["errors"]), flush=True)
stop.set()
for t in threads: t.join()
bad = 0
for name in live:
    try:
        with open(os.path.join(a.root, name), "rb") as f:
            if f.read() != body(name): bad += 1; errs.append("final mismatch " + name)
    except OSError as e:
        bad += 1; errs.append("final %s: %s" % (name, e))
with open(a.root.rstrip("/") + ".live", "w") as f: f.write("\n".join(live) + "\n")
print("final live=%d verified_bad=%d mismatch=%d errors=%d" % (len(live), bad, st["mismatch"], st["errors"]))
for e in errs[:20]: print("ERR", e)
sys.exit(1 if bad or st["mismatch"] or st["errors"] else 0)
