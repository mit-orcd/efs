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
any version; a read matching none is re-read RETRIES times, RETRY_WAIT_S
apart (another client's open-while-writing may legitimately show a partial
state). A read that settles on a version is counted as "settled"; one that
never settles is a mismatch ("unsettled"), never a transient. Both save
the first non-matching bytes (--mismatch-dir) with the same sidecar as a
local mismatch, plus whether and after how long the read settled.

Evidence (--mismatch-dir DIR): every mismatching read - local or remote -
saves the bytes it returned as DIR/N/NAME with
DIR/N/meta.json: source, expected version (remote: the settled version or
null, with every candidate version's length and hash), expected and actual
lengths and sha256, first differing offset, inode, read time, host, argv
and, for a remote read, settled/settle_time/settle_after_s/retries. --keep-dir hard-links the file there (on the same mount) so a deleter
cannot destroy its server rows. --mgmt captures raft-getattr and
raft-getchunks for the inode at that moment. Artifact-write failures are
recorded in meta.json and printed. The first SAVE_CAP reads per source are
saved; a counter alone never stands in for a saved read.
"""
import argparse, errno, hashlib, json, math, os, random, socket, subprocess, sys, threading, time

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
ap.add_argument("--mismatch-dir", help="save each mismatching read (local, remote) as DIR/N/NAME "
               "+ DIR/N/meta.json (N counts saved reads) for w125_modify_inspect.py DIR/N NAME")
ap.add_argument("--keep-dir", help="hard-link each mismatching file here (same mount)")
ap.add_argument("--mgmt", help="'EFS-MGMT ADDR' to capture raft-getattr/raft-getchunks at a mismatch")

RETRIES, RETRY_WAIT_S, SAVE_CAP = 3, 2.0, 20
STATUS_INTERVAL_S = 10
a = None
lock = threading.Lock()
live, stop = [], threading.Event()
st = dict(written=0, wbytes=0, read=0, rbytes=0, enoent=0, deleted=0, mismatch=0, errors=0,
          modified=0, settled=0, unsettled=0, newer=0, saved=0, artifact_errors=0)
ver, flocks = {}, {}            # name -> current version, name -> lock (this client)
errs = []
saved = [0]                     # next DIR/N
saved_by = {}                   # source -> saved count (SAVE_CAP each)
seen = set(); gone = []

def configure(argv):
    """Parse argv and reset the run state (tests call this, then the pieces)."""
    global a
    a = ap.parse_args(argv)
    os.makedirs(a.root, exist_ok=True)
    del live[:], errs[:], gone[:]
    stop.clear(); seen.clear(); ver.clear(); flocks.clear(); saved_by.clear(); saved[0] = 0
    for k in st: st[k] = 0
    return a

def flock(name):
    with lock:
        l = flocks.get(name)
        if l is None: l = flocks[name] = threading.Lock()
        return l
def bump(k, n=1):
    with lock: st[k] += n
def fail(kind, msg):
    with lock:
        st[kind] += 1
        if len(errs) < 20: errs.append("%s %s" % (kind, msg))

def sha(b): return hashlib.sha256(b).hexdigest()
def first_diff(got, want):
    return next((i for i in range(min(len(got), len(want))) if got[i] != want[i]), min(len(got), len(want)))

def write_meta(d, meta):
    with open(os.path.join(d, "meta.json"), "w") as f: json.dump(meta, f, indent=1, sort_keys=True)

def save_mismatch(name, got, v, want, source="local", read_time=None, locked=True):
    """Keep the bytes a mismatching read returned: a transient view is gone by
    the time the store can be inspected. Saved under --mismatch-dir (outside
    Git) as DIR/N/NAME plus DIR/N/meta.json; returns (N, dir, meta) or None.
    v/want: the version the read was checked against and its bytes (local and
    final); None for a remote read, which may match any version - then every
    version's length and hash is listed as a candidate and `version` is
    filled in by settle(). The caller holds the file's lock (locked=True), so
    the version and the re-read copy under --keep-dir belong to this read.
    --keep-dir hard-links the file there (on the same mount) so a deleter
    cannot destroy its server rows. --mgmt: 'MGMT raft-getattr INO' and
    'MGMT raft-getchunks INO', saved next to the bytes."""
    if not a.mismatch_dir: return None
    with lock:
        k = saved_by.get(source, 0)
        if k >= SAVE_CAP: return None
        saved_by[source] = k + 1; saved[0] += 1; n = saved[0]; st["saved"] += 1
    d = os.path.join(a.mismatch_dir, str(n)); path = os.path.join(d, name)
    meta = dict(name=name, source=source, locked=locked, read_time=read_time or time.time(),
                save_time=time.time(), host=socket.gethostname(), argv=sys.argv, root=a.root,
                actual_len=len(got), actual_sha256=sha(got), capture=path)
    if want is not None:
        meta.update(version=v, expected_len=len(want), expected_sha256=sha(want), first_diff=first_diff(got, want))
    else:
        cands = [contents(name, i) for i in range(MAX_VERSIONS + 1)]
        meta.update(version=None, candidates=[dict(version=i, len=len(c), sha256=sha(c)) for i, c in enumerate(cands)],
                    settled=None, settle_time=None, settle_after_s=None, retries=0)
        # Nearest candidate by length: what to compare the bytes against first.
        near = min(range(MAX_VERSIONS + 1), key=lambda i: (abs(len(cands[i]) - len(got)), i))
        meta.update(expected_len=len(cands[near]), expected_sha256=sha(cands[near]),
                    first_diff=first_diff(got, cands[near]), nearest_version=near)
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f: f.write(got)
        src = os.path.join(a.root, name)
        meta["ino"] = os.stat(src).st_ino
        if a.keep_dir:
            os.makedirs(a.keep_dir, exist_ok=True)
            os.link(src, os.path.join(a.keep_dir, str(n)))
            meta["kept"] = os.path.join(a.keep_dir, str(n))
        if a.mgmt:
            for cmd in ("raft-getattr", "raft-getchunks"):
                m = a.mgmt.split()
                r = subprocess.run([m[0], cmd] + m[1:] + [str(meta["ino"])],
                                   capture_output=True, text=True, timeout=30)
                with open(os.path.join(d, cmd + ".txt"), "w") as f:
                    f.write(r.stdout + r.stderr + "rc=%d\n" % r.returncode)
    except (OSError, subprocess.SubprocessError) as e:
        meta["artifact_error"] = str(e); bump("artifact_errors")
        print("mismatch artifact error %s: %s" % (name, e), file=sys.stderr, flush=True)
    try:
        write_meta(d, meta)
    except OSError as e:
        bump("artifact_errors")
        print("mismatch artifact error %s: meta.json: %s" % (name, e), file=sys.stderr, flush=True)
    return n, d, meta

def settle(rec, v, retries, settle_time):
    """Record how a saved remote read ended: settled on version v after
    `retries` re-reads (v None: never settled)."""
    if rec is None: return
    n, d, meta = rec
    meta.update(settled=v is not None, version=v, retries=retries, settle_time=settle_time,
                settle_after_s=settle_time - meta["read_time"])
    if v is not None:
        want = contents(meta["name"], v)
        meta.update(expected_len=len(want), expected_sha256=sha(want), first_diff=first_diff(contents_of(d, meta), want))
    try: write_meta(d, meta)
    except OSError as e:
        bump("artifact_errors")
        print("mismatch artifact error %s: meta.json: %s" % (meta["name"], e), file=sys.stderr, flush=True)
def contents_of(d, meta):
    try:
        with open(meta["capture"], "rb") as f: return f.read()
    except OSError: return b""

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
def remote_check(name):
    """One remote verification of NAME: read, accept any version, re-read up
    to RETRIES times while nothing matches. Returns "ok", "settled",
    "unsettled" or "enoent"; the first non-matching bytes are saved either
    way and the sidecar says whether and when the read settled."""
    try:
        t = time.time(); got = read_file(name); v = any_version(name, got)
    except FileNotFoundError:
        bump("enoent"); return "enoent"
    except OSError as e:
        fail("errors", "read %s: %s" % (name, e)); return "error"
    if v is not None:
        bump("read"); bump("rbytes", len(got))
        if v: bump("newer")
        return "ok"
    rec = save_mismatch(name, got, None, None, source="remote", read_time=t, locked=False)
    tries = 0
    while v is None and tries < RETRIES:
        time.sleep(RETRY_WAIT_S); tries += 1
        try: got = read_file(name); v = any_version(name, got)
        except FileNotFoundError:
            settle(rec, None, tries, time.time()); bump("enoent"); return "enoent"
        except OSError as e:
            settle(rec, None, tries, time.time()); fail("errors", "read %s: %s" % (name, e)); return "error"
    settle(rec, v, tries, time.time())
    if v is None:
        fail("mismatch", "%s: %d bytes match no version after %d re-reads (unsettled)" % (name, len(got), tries))
        bump("unsettled"); return "unsettled"
    bump("settled"); bump("read"); bump("rbytes", len(got))
    if v: bump("newer")
    return "settled"
def reader(_):
    while not stop.is_set():
        with lock: name = random.choice(live) if live else None
        if not name: time.sleep(0.05); continue
        if a.remote:
            remote_check(name); continue
        l = flock(name)
        if not l.acquire(blocking=False): continue
        try:
            want = contents(name, ver.get(name, 0))
            try: t = time.time(); got = read_file(name)
            except FileNotFoundError: bump("enoent"); continue
            except OSError as e: fail("errors", "read %s: %s" % (name, e)); continue
            if got != want:
                fail("mismatch", "%s v%d: %d bytes, expected %d" % (name, ver.get(name, 0), len(got), len(want)))
                save_mismatch(name, got, ver.get(name, 0), want, source="local", read_time=t)
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

def status_line(t0):
    with lock: s = dict(st); n = len(live)
    return ("t=%.0fs live=%d written=%d (%.0f MiB) read=%d (%.0f MiB) enoent=%d deleted=%d modified=%d newer=%d "
            "settled=%d unsettled=%d mismatch=%d errors=%d saved=%d artifact_errors=%d" % (
        time.time() - t0, n, s["written"], s["wbytes"] / 2**20, s["read"], s["rbytes"] / 2**20,
        s["enoent"], s["deleted"], s["modified"], s["newer"], s["settled"], s["unsettled"], s["mismatch"],
        s["errors"], s["saved"], s["artifact_errors"]))

def final_verify():
    """Verify every live file once more. Returns (bad, missing)."""
    bad, missing = 0, []
    for name in live:
        l = flock(name) if not a.remote else None
        if l: l.acquire()
        try:
            t = time.time(); got = read_file(name)
            if a.remote:
                v = any_version(name, got); ok = v is not None; want = None
            else:
                v = ver.get(name, 0); want = contents(name, v); ok = got == want
            if not ok:
                bad += 1; errs.append("final mismatch %s v%s: %d bytes" % (name, v if want is not None else "?", len(got)))
        except FileNotFoundError:
            missing.append(name)
        except OSError as e:
            bad += 1; errs.append("final %s: %s" % (name, e))
        finally:
            if l: l.release()
    return bad, missing

def verdict(bad):
    return 1 if bad or st["mismatch"] or st["errors"] else 0

def main(argv=None):
    configure(sys.argv[1:] if argv is None else argv)
    threads = [] if a.remote else [threading.Thread(target=writer, args=(i,)) for i in range(a.writers)]
    if a.remote: threads.append(threading.Thread(target=walker))
    threads += [threading.Thread(target=reader, args=(i,)) for i in range(a.readers)]
    threads += [threading.Thread(target=deleter, args=(i,)) for i in range(a.deleters)]
    threads += [threading.Thread(target=modifier, args=(i,)) for i in range(0 if a.remote else a.modifiers)]
    for t in threads: t.start()
    t0 = time.time()
    while time.time() - t0 < a.seconds:
        time.sleep(max(0.05, min(STATUS_INTERVAL_S, a.seconds - (time.time() - t0))))
        print(status_line(t0), flush=True)
    stop.set()
    for t in threads: t.join()
    bad, missing = final_verify()
    tag = ".remote" if a.remote else ""
    with open(a.root.rstrip("/") + tag + ".live", "w") as f: f.write("\n".join(live) + "\n")
    with open(a.root.rstrip("/") + tag + ".missing", "w") as f: f.write("\n".join(missing) + "\n")
    if a.deleted_out:
        with open(a.deleted_out, "w") as f: f.write("\n".join(gone) + "\n")
    print("final live=%d verified_bad=%d missing=%d mismatch=%d errors=%d deleted=%d settled=%d unsettled=%d "
          "saved=%d artifact_errors=%d" % (
        len(live), bad, len(missing), st["mismatch"], st["errors"], st["deleted"], st["settled"],
        st["unsettled"], st["saved"], st["artifact_errors"]))
    for e in errs[:20]: print("ERR", e)
    return verdict(bad)

if __name__ == "__main__":
    sys.exit(main())
