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

Evidence (--mismatch-dir DIR): every mismatching read - local, remote or
final verification - saves the bytes it returned as DIR/N/NAME with
DIR/N/meta.json: source, expected version (remote: the settled version or
null, with every candidate version's length and hash), expected and actual
lengths and sha256, first differing offset, inode, read time, host, argv
and, for a remote read, settled/settle_time/settle_after_s/retries. With
--keep-dir the file is re-read while the per-file lock is still held and
the bytes are written to KEEP/N by exclusive create (a copy, not a hard
link: a later modifier cannot change it); meta.json records the copy's
length and hash and the re-read's. --mgmt captures raft-getattr and
raft-getchunks for the inode at that moment. Artifact-write failures are
recorded in meta.json and printed. The first SAVE_CAP reads per source are
saved; a counter alone never stands in for a saved read.

Operation-aware modify oracle (W155): a modify is a sequence of syscalls
(open, truncate, write in WRITE_CHUNK pieces, close; close(2) is where FUSE
FLUSH, i.e. the client's flush-meta, runs), and an error promises neither
rollback nor completion. Every attempt that fails records its stages
(stage, start/end time, errno, bytes), the failing stage, bytes submitted
of the total, the inode and the attempted content's length and sha256.
The file becomes UNRESOLVED and is quarantined: no modifier or deleter
touches it again. Its candidate states are the contents at the syscall
boundaries actually reached - the last known successful version (base),
the state after each completed call, and the full effect of the failing
call - never a blanket old-or-new. A read of an unresolved file that
matches a candidate is recorded as an observation; mixed or foreign bytes
are a mismatch. Final verification (writers stopped) requires exactly the
last known successful version of every resolved file and classifies each
unresolved file as base, attempted, intermediate:<boundary>, foreign
(a mismatch), missing or unread. Creates and unlinks that fail are
recorded the same way (stage, time, inode where known). --modify-log FILE
gets one JSON line per failed attempt and per final classification, for
scripts/w155_correlate.py. Exit status: 0 PASS (no error, mismatch or
unresolved file), 1 FAIL (any mismatch or error, which includes every
failed modify), 5 INCOMPLETE (nothing failed, but something is unresolved
or unclassified); errors and unresolved outcomes are never a PASS.
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
ap.add_argument("--mismatch-dir", help="save each mismatching read (local, remote, final) as DIR/N/NAME "
               "+ DIR/N/meta.json (N counts saved reads) for w125_modify_inspect.py DIR/N NAME")
ap.add_argument("--keep-dir", help="copy each mismatching file here (same mount) under the per-file lock")
ap.add_argument("--mgmt", help="'EFS-MGMT ADDR' to capture raft-getattr/raft-getchunks at a mismatch")
ap.add_argument("--modify-log", help="one JSON line per failed modify/create/unlink attempt and per final "
               "classification of an unresolved file (scripts/w155_correlate.py OUT)")

RETRIES, RETRY_WAIT_S, SAVE_CAP = 3, 2.0, 20
WRITE_CHUNK = 1 << 20           # bytes per write(2) of a modify: "bytes submitted" counts whole calls
INCOMPLETE_RC = 5
STATUS_INTERVAL_S = 10
a = None
lock = threading.Lock()
live, stop = [], threading.Event()
st = dict(written=0, wbytes=0, read=0, rbytes=0, enoent=0, deleted=0, mismatch=0, errors=0,
          modified=0, settled=0, unsettled=0, newer=0, saved=0, artifact_errors=0,
          unresolved=0, unresolved_reads=0, cls_base=0, cls_attempted=0, cls_intermediate=0,
          cls_foreign=0, cls_missing=0, cls_unread=0)
ver, flocks = {}, {}            # name -> current version, name -> lock (this client)
errs = []
saved = [0]                     # next DIR/N
saved_by = {}                   # source -> saved count (SAVE_CAP each)
seen = set(); gone = []
fault = None                    # self-test hook: fault(rec, stage, fn) runs (or skips) fn and may raise
unresolved = {}                 # name -> failed attempt record (+ "cands": [(label, bytes)]), quarantined
attempts = []                   # every failed attempt record, in order

def configure(argv):
    """Parse argv and reset the run state (tests call this, then the pieces)."""
    global a
    a = ap.parse_args(argv)
    os.makedirs(a.root, exist_ok=True)
    del live[:], errs[:], gone[:], attempts[:]
    stop.clear(); seen.clear(); ver.clear(); flocks.clear(); saved_by.clear(); saved[0] = 0
    unresolved.clear()
    if a.modify_log: open(a.modify_log, "w").close()
    for k in st: st[k] = 0
    return a

def flock(name):
    with lock:
        l = flocks.get(name)
        if l is None: l = flocks[name] = threading.Lock()
        return l
def bump(k, n=1):
    with lock: st[k] += n
def utc(t): return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(t)) + ".%03dZ" % int(t % 1 * 1000)
def fail(kind, msg, t=None):
    with lock:
        st[kind] += 1
        if len(errs) < 20: errs.append("%s %s @%s" % (kind, msg, utc(t or time.time())))
def log_record(rec):
    """Append one JSON line to --modify-log (raw evidence, outside Git)."""
    if not a.modify_log: return
    line = json.dumps({k: v for k, v in rec.items() if k != "cands"}, sort_keys=True)
    with lock:
        try:
            with open(a.modify_log, "a") as f: f.write(line + "\n")
        except OSError as e:
            st["artifact_errors"] += 1
            print("modify-log error: %s" % e, file=sys.stderr, flush=True)

def sha(b): return hashlib.sha256(b).hexdigest()
def first_diff(got, want):
    return next((i for i in range(min(len(got), len(want))) if got[i] != want[i]), min(len(got), len(want)))

def write_meta(d, meta):
    with open(os.path.join(d, "meta.json"), "w") as f: json.dump(meta, f, indent=1, sort_keys=True)

def save_mismatch(name, got, v, want, source="local", read_time=None, locked=True, cands=None):
    """Keep the bytes a mismatching read returned: a transient view is gone by
    the time the store can be inspected. Saved under --mismatch-dir (outside
    Git) as DIR/N/NAME plus DIR/N/meta.json; returns (N, dir, meta) or None.
    v/want: the version the read was checked against and its bytes (local and
    final); None for a remote read, which may match any version - then every
    version's length and hash is listed as a candidate and `version` is
    filled in by settle(). cands: an unresolved file's candidate states
    [(label, bytes)] (version None, unresolved=True, those are the candidates).
    The caller holds the file's lock (locked=True), so
    the version and the re-read copy under --keep-dir belong to this read.
    --keep-dir: re-read the file now and write those bytes to KEEP/N by
    exclusive create, a copy a modifier cannot change afterwards (a hard link
    would follow later versions). --mgmt: 'MGMT raft-getattr INO' and
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
    if cands is not None:
        meta.update(version=None, unresolved=True,
                    candidates=[dict(label=lb, len=len(c), sha256=sha(c)) for lb, c in cands])
        near = min(range(len(cands)), key=lambda i: (abs(len(cands[i][1]) - len(got)), i))
        meta.update(expected_len=len(cands[near][1]), expected_sha256=sha(cands[near][1]),
                    first_diff=first_diff(got, cands[near][1]), nearest_candidate=cands[near][0])
    elif want is not None:
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
            with open(src, "rb") as f: again = f.read()
            meta.update(reread_len=len(again), reread_sha256=sha(again), reread_same_as_read=again == got)
            kept = os.path.join(a.keep_dir, str(n))
            fd = os.open(kept, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o444)
            with os.fdopen(fd, "wb") as f: f.write(again)
            meta.update(kept=kept, kept_len=len(again), kept_sha256=sha(again))
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

def stage(rec, name, fn, **kw):
    """Run one syscall stage of an attempt and append it to rec["stages"]
    (start/end time, errno, extra fields); an OSError marks the failing stage."""
    t0 = time.time()
    try:
        r = fault(rec, name, fn) if fault else fn()
    except OSError as e:
        rec["stages"].append(dict(stage=name, t0=t0, t1=time.time(), errno=e.errno, error=str(e), **kw))
        rec.update(failed_stage=name, errno=e.errno, error=str(e), t_fail=t0)
        raise
    rec["stages"].append(dict(stage=name, t0=t0, t1=time.time(), errno=0, **kw))
    return r

def close_fd(rec, fd):
    """close(2) as a stage. After an earlier failing stage the close is still
    recorded, but the first failure stays the failing stage (the caller's
    exception propagates)."""
    if rec["failed_stage"] is None:
        return stage(rec, "close", lambda: os.close(fd))
    first = {k: rec[k] for k in ("failed_stage", "errno", "error", "t_fail")}
    try: stage(rec, "close", lambda: os.close(fd))
    except OSError: pass
    rec.update(first)

def attempt_record(kind, name, **kw):
    rec = dict(kind=kind, name=name, ino=None, t_start=time.time(), stages=[], failed_stage=None,
               errno=None, error=None, t_fail=None, host=socket.gethostname())
    rec.update(kw); return rec

def write_all(fd, rec, data, off, state, cands, total):
    """write(2) DATA at OFF in WRITE_CHUNK calls; each completed call adds a
    candidate boundary state, a failing call adds its full effect. Returns state."""
    k = 0; rec["bytes_total"] = total
    while k < len(data):
        piece = data[k:k + WRITE_CHUNK]
        full = state[:off + k] + piece + state[off + k + len(piece):]
        try:
            n = stage(rec, "write", lambda: os.write(fd, piece), offset=off + k, requested=len(piece),
                      submitted_before=rec["bytes_submitted"], total=total)
        except OSError:
            cands.append(("write %d+%d/%d (failing call)" % (rec["bytes_submitted"], len(piece), total), full))
            raise
        rec["stages"][-1]["returned"] = n
        state = state[:off + k] + piece[:n] + state[off + k + n:]
        k += n; rec["bytes_submitted"] += n
        cands.append(("write %d/%d" % (rec["bytes_submitted"], total), state))
    return state

def writer(w):
    i = 0
    while not stop.is_set():
        name = "w%d/d%03d/f%06d" % (w, i % 50, i); i += 1
        path = os.path.join(a.root, name)
        data = body(name)
        rec = attempt_record("create", name, version=0, bytes_submitted=0, attempted_len=len(data),
                             attempted_sha256=sha(data))
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            fd = stage(rec, "open", lambda: os.open(path + ".tmp", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644))
            try:
                rec["ino"] = stage(rec, "fstat", lambda: os.fstat(fd)).st_ino
                write_all(fd, rec, data, 0, b"", [], len(data))
            finally:
                close_fd(rec, fd)
            stage(rec, "rename", lambda: os.rename(path + ".tmp", path))
            with lock: live.append(name); st["written"] += 1; st["wbytes"] += len(data)
        except OSError as e:
            attempts.append(rec); log_record(rec)
            fail("errors", "write %s stage=%s bytes=%d/%d ino=%s: %s" % (
                name, rec["failed_stage"], rec["bytes_submitted"], len(data), rec["ino"], e), rec["t_fail"])

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
            if name in unresolved:
                check_unresolved(name, got, t); continue
            if got != want:
                fail("mismatch", "%s v%d: %d bytes, expected %d" % (name, ver.get(name, 0), len(got), len(want)))
                save_mismatch(name, got, ver.get(name, 0), want, source="local", read_time=t)
            else: bump("read"); bump("rbytes", len(got))
        finally:
            l.release()
def cand_match(cands, got):
    return next((i for i, (_, c) in enumerate(cands) if c == got), None)
def check_unresolved(name, got, t, source="local"):
    """A read of a quarantined file: any recorded candidate state is
    acceptable for now (recorded as an observation); anything else - mixed
    or foreign bytes - is a mismatch. The caller holds the file's lock."""
    u = unresolved[name]; k = cand_match(u["cands"], got)
    if k is None:
        fail("mismatch", "%s unresolved v%d->v%d: %d bytes match no candidate state" % (
            name, u["base_version"], u["version"], len(got)), t)
        save_mismatch(name, got, None, None, source=source, read_time=t, cands=u["cands"])
        return False
    bump("unresolved_reads"); bump("read"); bump("rbytes", len(got))
    with lock:
        obs = u.setdefault("observed", {})
        o = obs.setdefault(u["cands"][k][0], dict(n=0, first=t)); o["n"] += 1; o["last"] = t
    return True

def truncate_to(old, n): return old[:n] + b"\0" * max(0, n - len(old))
def do_modify(path, name, v, op, old, rec):
    """Apply op_of(name, v) to the file as stages; returns the final state.
    rec["cands"] collects the boundary states [(label, bytes)], first the base."""
    cands = rec["cands"] = [("base v%d" % (v - 1), old)]; rec["bytes_total"] = 0
    fd = stage(rec, "open", lambda: os.open(path, os.O_RDWR))
    try:
        rec["ino"] = stage(rec, "fstat", lambda: os.fstat(fd)).st_ino
        state = old
        if op[0] in ("rewrite", "shrink", "extend"):
            n = 0 if op[0] == "rewrite" else int(len(old) * op[1]) if op[0] == "shrink" else len(old) + op[1]
            full = truncate_to(state, n)
            try: stage(rec, "truncate", lambda: os.ftruncate(fd, n), length=n)
            except OSError:
                cands.append(("truncate %d (failing call)" % n, full)); raise
            state = full; cands.append(("truncate %d" % n, state))
        if op[0] in ("rewrite", "append", "overwrite"):
            if op[0] == "rewrite": off, data = 0, op[1]
            elif op[0] == "append": off, data = len(old), op[1]
            else: off = int(len(old) * op[1]); data = op[2][:max(0, len(old) - off)]
            stage(rec, "seek", lambda: os.lseek(fd, off, os.SEEK_SET), offset=off)
            state = write_all(fd, rec, data, off, state, cands, len(data))
    finally:
        close_fd(rec, fd)
    return state

def modifier(_):
    while not stop.is_set():
        with lock: name = random.choice(live) if live else None
        if not name or ver.get(name, 0) >= MAX_VERSIONS or name in unresolved: time.sleep(0.01); continue
        l = flock(name)
        if not l.acquire(blocking=False): continue
        try:
            if name in unresolved: continue
            v = ver.get(name, 0) + 1; op = op_of(name, v); old = contents(name, v - 1); new = contents(name, v)
            rec = attempt_record("modify", name, version=v, base_version=v - 1, op=op[0], bytes_submitted=0,
                                 attempted_len=len(new), attempted_sha256=sha(new),
                                 base_len=len(old), base_sha256=sha(old))
            try:
                state = do_modify(os.path.join(a.root, name), name, v, op, old, rec)
            except FileNotFoundError:
                if rec["failed_stage"] == "open": continue   # a deleter won the race before any change
                quarantine(name, rec); continue
            except OSError:
                quarantine(name, rec); continue
            if state != new:
                fail("errors", "modify %s v%d: harness model diverged from the syscalls it issued" % (name, v))
                continue
            ver[name] = v; bump("modified")
        finally:
            l.release()

def quarantine(name, rec):
    """A failed modify: the file is unresolved and no modelled mutation touches
    it again. Its candidates are the boundary states this attempt reached
    (deduplicated, first label kept); the attempted version is labelled."""
    seen_c, cands = set(), []
    for lb, c in rec["cands"]:
        h = sha(c)
        if h in seen_c: continue
        seen_c.add(h)
        if h == rec["attempted_sha256"]: lb += " = attempted v%d" % rec["version"]
        cands.append((lb, c))
    rec["cands"] = cands
    rec["candidates"] = [dict(label=lb, len=len(c), sha256=sha(c)) for lb, c in cands]
    with lock: unresolved[name] = rec; attempts.append(rec); st["unresolved"] += 1
    log_record(rec)
    fail("errors", "modify %s v%d %s stage=%s bytes=%d/%d ino=%s: %s" % (
        name, rec["version"], rec["op"], rec["failed_stage"], rec["bytes_submitted"], rec["bytes_total"],
        rec["ino"], rec["error"]), rec["t_fail"])

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
            else:
                i = random.randrange(len(live)); name = live[i]
                if name in unresolved: name = None      # quarantined: never deleted
                else: live.pop(i)
        if not name: time.sleep(0.05); continue
        l = flock(name) if not a.remote else None
        if l: l.acquire()
        try:
            if name in unresolved:                       # failed a modify while we waited
                with lock: live.append(name)
                continue
            rec = attempt_record("unlink", name)
            stage(rec, "unlink", lambda: os.unlink(os.path.join(a.root, name))); bump("deleted")
            if a.remote:
                with lock: gone.append(name)
        except FileNotFoundError:
            bump("enoent")
        except OSError as e:
            attempts.append(rec); log_record(rec)
            fail("errors", "unlink %s: %s" % (name, e), rec["t_fail"])
        finally:
            if l: l.release()

def status_line(t0):
    with lock: s = dict(st); n = len(live)
    return ("t=%.0fs live=%d written=%d (%.0f MiB) read=%d (%.0f MiB) enoent=%d deleted=%d modified=%d newer=%d "
            "settled=%d unsettled=%d mismatch=%d errors=%d unresolved=%d saved=%d artifact_errors=%d" % (
        time.time() - t0, n, s["written"], s["wbytes"] / 2**20, s["read"], s["rbytes"] / 2**20,
        s["enoent"], s["deleted"], s["modified"], s["newer"], s["settled"], s["unsettled"], s["mismatch"],
        s["errors"], s["unresolved"], s["saved"], s["artifact_errors"]))

def classify(name, got, t):
    """Final classification of an unresolved file against its candidates:
    base (the last known successful version), attempted, intermediate:<label>,
    or foreign (no candidate: a mismatch, saved). Recorded in the attempt."""
    u = unresolved[name]; k = cand_match(u["cands"], got)
    if k is None: cls, label = "foreign", None
    else:
        label = u["cands"][k][0]
        cls = "base" if k == 0 else "attempted" if sha(got) == u["attempted_sha256"] else "intermediate:" + label
    u["final"] = dict(classification=cls, label=label, len=len(got), sha256=sha(got), time=t)
    return cls

def final_unresolved(name, t, got=None, err=None):
    u = unresolved[name]
    if err is not None:
        cls = "missing" if isinstance(err, FileNotFoundError) else "unread"
        u["final"] = dict(classification=cls, error=str(err), time=t)
    else:
        cls = classify(name, got, t)
    bump("cls_" + cls.split(":")[0])
    log_record(dict(kind="final-classification", name=name, ino=u["ino"], version=u["version"],
                    base_version=u["base_version"], failed_stage=u["failed_stage"],
                    observed=u.get("observed", {}), **u["final"]))
    if cls == "foreign":
        errs.append("final unresolved %s v%d->v%d: %d bytes match no candidate state" % (
            name, u["base_version"], u["version"], len(got)))
        save_mismatch(name, got, None, None, source="final", read_time=t, cands=u["cands"])
    return cls

def final_verify():
    """Verify every live file once more (writers stopped): a resolved file
    must be exactly its last known successful version; an unresolved file is
    classified (final_unresolved). Mismatches are saved like any other
    (source "final"). Returns (bad, missing); bad counts foreign unresolved
    files too, missing excludes unresolved files (classified "missing")."""
    bad, missing = 0, []
    for name in live:
        l = flock(name) if not a.remote else None
        if l: l.acquire()
        try:
            t = time.time()
            if name in unresolved:
                try: got = read_file(name)
                except OSError as e: final_unresolved(name, t, err=e); continue
                if final_unresolved(name, t, got) == "foreign": bad += 1
                continue
            got = read_file(name)
            if a.remote:
                v = any_version(name, got); ok = v is not None; want = None
            else:
                v = ver.get(name, 0); want = contents(name, v); ok = got == want
            if not ok:
                bad += 1; errs.append("final mismatch %s v%s: %d bytes" % (name, v if want is not None else "?", len(got)))
                save_mismatch(name, got, v if want is not None else None, want, source="final", read_time=t,
                              locked=l is not None)
        except FileNotFoundError:
            missing.append(name)
        except OSError as e:
            bad += 1; errs.append("final %s: %s" % (name, e))
        finally:
            if l: l.release()
    return bad, missing

def verdict(bad):
    """0 PASS, 1 FAIL (any mismatch or error), INCOMPLETE_RC when nothing
    failed but an outcome is unresolved or unclassified. Never PASS with an
    error or an unresolved file."""
    if bad or st["mismatch"] or st["errors"]: return 1
    if st["unresolved"] or unresolved or st["cls_missing"] or st["cls_unread"]: return INCOMPLETE_RC
    return 0
def verdict_name(rc): return {0: "PASS", 1: "FAIL", INCOMPLETE_RC: "INCOMPLETE"}[rc]

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
    rc = verdict(bad)
    print("final live=%d verified_bad=%d missing=%d mismatch=%d errors=%d deleted=%d settled=%d unsettled=%d "
          "saved=%d artifact_errors=%d unresolved=%d unresolved_reads=%d" % (
        len(live), bad, len(missing), st["mismatch"], st["errors"], st["deleted"], st["settled"],
        st["unsettled"], st["saved"], st["artifact_errors"], st["unresolved"], st["unresolved_reads"]))
    print("classified base=%d attempted=%d intermediate=%d foreign=%d missing=%d unread=%d" % (
        st["cls_base"], st["cls_attempted"], st["cls_intermediate"], st["cls_foreign"], st["cls_missing"],
        st["cls_unread"]))
    for name, u in sorted(unresolved.items()):
        f = u.get("final", {})
        print("UNRESOLVED %s ino=%s v%d->v%d %s stage=%s errno=%s bytes=%d/%d at=%s observed=%s final=%s%s" % (
            name, u["ino"], u["base_version"], u["version"], u["op"], u["failed_stage"], u["errno"],
            u["bytes_submitted"], u["bytes_total"], utc(u["t_fail"]) if u["t_fail"] else "?",
            ",".join("%s:%d" % (k, o["n"]) for k, o in sorted(u.get("observed", {}).items())) or "-",
            f.get("classification", "unverified"), " (%s)" % f["label"] if f.get("label") else ""))
    for e in errs[:20]: print("ERR", e)
    print("verdict=%s" % verdict_name(rc))
    return rc

if __name__ == "__main__":
    sys.exit(main())
