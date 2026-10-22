#!/usr/bin/env python3
"""w125_concurrent.py ROOT SECONDS [--writers 4] [--readers 4] [--deleters 2] [--modifiers 0] [--remote --deleted-out FILE]

Concurrent writers, readers and deleters on one efs mount, for packing and
compaction under load. Every file's content is a deterministic function of
its name (sizes log-uniform from 1 B to 4 MiB, so most are small and
packed, some are large), so any read can be checked:
  writers   create new files (written, closed, then published to the live set);
  readers   read a random live file and compare it with its expected bytes;
            ENOENT is fine (a deleter won the race), wrong bytes are not;
  deleters  unlink a random live file (kept to about half the files written).
Writers publish a file atomically: written and closed as NAME.tmp, then
renamed to NAME. Every 10 s one status line. After SECONDS the workers
stop and every remaining file is verified once more; a file that is gone
is recorded in ROOT.missing (another client may have deleted it) rather
than counted as an error. The final live list is written to ROOT.live.
--remote: a second client on another mount, with no writers: it discovers
published files by listing ROOT every 5 s, verifies them with its readers
and deletes some with its deleters (about one per two discovered), naming
each in --deleted-out. Exit 1 on any mismatch or unexpected error.
--modifiers N: N threads modify live files in place (W125 closure C1): each
version applies one operation to the previous content - rewrite, append,
truncate shorter, truncate longer, overwrite in the middle - so a file's
content is a function of (name, version), at most MAX_VERSIONS versions.
On this client readers, deleters and modifiers share a per-file lock, so
reads are checked against the current version. A --remote reader accepts
any version; a read matching none is re-read 3 times over ~6 s (another
client's open-while-writing may legitimately show a partial state) and only
a state that never settles counts as a mismatch ("transient" otherwise).
"""
import argparse, errno, math, os, random, sys, threading, time

import functools
MAX_VERSIONS = 6
def body(name):
    r = random.Random(name)
    size = int(math.exp(r.uniform(0, math.log(4 << 20))))
    return r.randbytes(max(1, size))
def op_of(name, v):
    """The in-place operation that makes version v from version v-1."""
    r = random.Random("%s#%d" % (name, v)); k = r.randrange(5)
    if k == 0: return ("rewrite", body("%s#%d" % (name, v)))
    if k == 1: return ("append", r.randbytes(r.randint(1, 20000)))
    if k == 2: return ("shrink", r.uniform(0.1, 0.9))
    if k == 3: return ("extend", r.randint(1, 8000))
    return ("overwrite", r.random(), r.randbytes(r.randint(1, 4000)))
def apply_op(old, op):
    if op[0] == "rewrite": return op[1]
    if op[0] == "append": return old + op[1]
    if op[0] == "shrink": return old[:int(len(old) * op[1])]
    if op[0] == "extend": return old + b"\0" * op[1]
    off = int(len(old) * op[1]); x = op[2][:max(0, len(old) - off)]
    return old[:off] + x + old[off + len(x):]
@functools.lru_cache(maxsize=512)
def contents(name, v):
    return body(name) if v == 0 else apply_op(contents(name, v - 1), op_of(name, v))

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("root"); ap.add_argument("seconds", type=float)
ap.add_argument("--writers", type=int, default=4); ap.add_argument("--readers", type=int, default=4)
ap.add_argument("--deleters", type=int, default=2)
ap.add_argument("--remote", action="store_true"); ap.add_argument("--deleted-out")
ap.add_argument("--modifiers", type=int, default=0)
ap.add_argument("--mismatch-dir", help="save each mismatching local read as DIR/N/NAME "
               "(N counts mismatches) for w125_modify_inspect.py DIR/N NAME")
a = ap.parse_args()
os.makedirs(a.root, exist_ok=True)
lock = threading.Lock()
live, stop = [], threading.Event()
st = dict(written=0, wbytes=0, read=0, rbytes=0, enoent=0, deleted=0, mismatch=0, errors=0,
          modified=0, transient=0, newer=0)
ver, flocks = {}, {}            # name -> current version, name -> lock (this client)
def flock(name):
    with lock:
        l = flocks.get(name)
        if l is None: l = flocks[name] = threading.Lock()
        return l
errs = []
def bump(k, n=1):
    with lock: st[k] += n
def fail(kind, msg):
    with lock:
        st[kind] += 1
        if len(errs) < 20: errs.append("%s %s" % (kind, msg))

saved = [0]
def save_mismatch(name, got):
    """Keep the bytes this read returned: a transient mismatch is gone by the
    time the store can be inspected. Saved under --mismatch-dir (outside Git)."""
    if not a.mismatch_dir: return
    with lock:
        saved[0] += 1; n = saved[0]
    if n > 20: return
    path = os.path.join(a.mismatch_dir, str(n), name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f: f.write(got)

def writer(w):
    i = 0
    while not stop.is_set():
        name = "w%d/d%03d/f%06d" % (w, i % 50, i); i += 1
        path = os.path.join(a.root, name)
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            data = body(name)
            with open(path + ".tmp", "wb") as f: f.write(data)
            os.rename(path + ".tmp", path)
            with lock: live.append(name); st["written"] += 1; st["wbytes"] += len(data)
        except OSError as e:
            fail("errors", "write %s: %s" % (name, e))

def read_file(name):
    with open(os.path.join(a.root, name), "rb") as f: return f.read()
def any_version(name, got):
    return next((v for v in range(MAX_VERSIONS + 1) if contents(name, v) == got), None)
def reader(_):
    while not stop.is_set():
        with lock: name = random.choice(live) if live else None
        if not name: time.sleep(0.05); continue
        if a.remote:
            try:
                got = read_file(name); v = any_version(name, got)
                for _ in range(3):
                    if v is not None: break
                    bump("transient"); time.sleep(2); got = read_file(name); v = any_version(name, got)
            except FileNotFoundError:
                bump("enoent"); continue
            except OSError as e:
                fail("errors", "read %s: %s" % (name, e)); continue
            if v is None: fail("mismatch", "%s: %d bytes match no version" % (name, len(got)))
            else:
                bump("read"); bump("rbytes", len(got))
                if v: bump("newer")
            continue
        l = flock(name)
        if not l.acquire(blocking=False): continue
        try:
            want = contents(name, ver.get(name, 0))
            try: got = read_file(name)
            except FileNotFoundError: bump("enoent"); continue
            except OSError as e: fail("errors", "read %s: %s" % (name, e)); continue
            if got != want:
                fail("mismatch", "%s v%d: %d bytes, expected %d" % (name, ver.get(name, 0), len(got), len(want)))
                save_mismatch(name, got)
            else: bump("read"); bump("rbytes", len(got))
        finally:
            l.release()
def modifier(_):
    while not stop.is_set():
        with lock: name = random.choice(live) if live else None
        if not name or ver.get(name, 0) >= MAX_VERSIONS: time.sleep(0.01); continue
        l = flock(name)
        if not l.acquire(blocking=False): continue
        try:
            v = ver.get(name, 0) + 1; op = op_of(name, v); old = contents(name, v - 1)
            try:
                with open(os.path.join(a.root, name), "r+b") as f:
                    if op[0] == "rewrite": f.truncate(0); f.write(op[1])
                    elif op[0] == "append": f.seek(0, 2); f.write(op[1])
                    elif op[0] == "shrink": f.truncate(int(len(old) * op[1]))
                    elif op[0] == "extend": f.truncate(len(old) + op[1])
                    else:
                        off = int(len(old) * op[1]); f.seek(off); f.write(op[2][:max(0, len(old) - off)])
            except FileNotFoundError:
                continue
            except OSError as e:
                fail("errors", "modify %s v%d %s: %s" % (name, v, op[0], e)); continue
            ver[name] = v; bump("modified")
        finally:
            l.release()

seen = set(); gone = []
def walker():
    while not stop.is_set():
        found = []
        for d, _, fs in os.walk(a.root):
            found += [os.path.relpath(os.path.join(d, f), a.root) for f in fs if not f.endswith(".tmp")]
        with lock:
            fresh = [n for n in found if n not in seen]
            seen.update(fresh); live.extend(fresh)
            st["written"] += len(fresh) # discovered, for the deleter ratio
        stop.wait(5)

def deleter(_):
    while not stop.is_set():
        with lock:
            if len(live) < 20 or st["deleted"] * 2 >= st["written"]: name = None
            else: name = live.pop(random.randrange(len(live)))
        if not name: time.sleep(0.05); continue
        l = flock(name) if not a.remote else None
        if l: l.acquire()
        try:
            os.unlink(os.path.join(a.root, name)); bump("deleted")
            if a.remote:
                with lock: gone.append(name)
        except FileNotFoundError:
            bump("enoent")
        except OSError as e:
            fail("errors", "unlink %s: %s" % (name, e))
        finally:
            if l: l.release()

threads = [] if a.remote else [threading.Thread(target=writer, args=(i,)) for i in range(a.writers)]
if a.remote: threads.append(threading.Thread(target=walker))
threads += [threading.Thread(target=reader, args=(i,)) for i in range(a.readers)]
threads += [threading.Thread(target=deleter, args=(i,)) for i in range(a.deleters)]
threads += [threading.Thread(target=modifier, args=(i,)) for i in range(0 if a.remote else a.modifiers)]
for t in threads: t.start()
t0 = time.time()
while time.time() - t0 < a.seconds:
    time.sleep(10)
    with lock: s = dict(st); n = len(live)
    print("t=%.0fs live=%d written=%d (%.0f MiB) read=%d (%.0f MiB) enoent=%d deleted=%d modified=%d newer=%d transient=%d mismatch=%d errors=%d" % (
        time.time() - t0, n, s["written"], s["wbytes"] / 2**20, s["read"], s["rbytes"] / 2**20,
        s["enoent"], s["deleted"], s["modified"], s["newer"], s["transient"], s["mismatch"], s["errors"]), flush=True)
stop.set()
for t in threads: t.join()
bad, missing = 0, []
for name in live:
    try:
        with open(os.path.join(a.root, name), "rb") as f:
            got = f.read()
            ok = any_version(name, got) is not None if a.remote else got == contents(name, ver.get(name, 0))
            if not ok: bad += 1; errs.append("final mismatch " + name)
    except FileNotFoundError:
        missing.append(name)
    except OSError as e:
        bad += 1; errs.append("final %s: %s" % (name, e))
tag = ".remote" if a.remote else ""
with open(a.root.rstrip("/") + tag + ".live", "w") as f: f.write("\n".join(live) + "\n")
with open(a.root.rstrip("/") + tag + ".missing", "w") as f: f.write("\n".join(missing) + "\n")
if a.deleted_out:
    with open(a.deleted_out, "w") as f: f.write("\n".join(gone) + "\n")
print("final live=%d verified_bad=%d missing=%d mismatch=%d errors=%d deleted=%d" % (
    len(live), bad, len(missing), st["mismatch"], st["errors"], st["deleted"]))
for e in errs[:20]: print("ERR", e)
sys.exit(1 if bad or st["mismatch"] or st["errors"] else 0)
