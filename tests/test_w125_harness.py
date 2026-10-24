#!/usr/bin/env python3
"""W125 modify-under-load harness self-tests: the failure paths of
scripts/w125_p0_cluster.sh, scripts/w125_modify_repro.sh and
scripts/w125_concurrent.py, with stubbed cluster helpers, fake daemons and
synthetic mismatches. No cluster, daemon or mount is started; everything
runs in a temporary directory (TMPDIR) and finishes in a few seconds.

Checks (docs/status/review-pack-validate-once-20261022.md, harness findings
2 and 3 and the remote-read point; devel-integration-review-20261022.md,
"W125 cleanup and remote/final evidence"):
  stop:    the real helper's stop with fake run-daemon processes: all orderly
           -> "stopped clean" rc 0; one that ignores TERM -> "needed KILL",
           rc 1; one that exited non-zero -> reported, rc 1; a client stop
           that fails -> reported, rc 1; W125_STOP_WAIT is honoured.
  wrapper: a stub cluster helper whose stop fails or needs KILL makes
           w125_modify_repro.sh exit 3, write "stop=forced" into done and
           keep the store; a failing workload keeps it too and exits 1;
           a clean stop after a passing (real, 1 s) workload deletes the
           store and writes PASS; a reused OUT is refused; every status is
           recorded in run.txt.
  sidecar: a synthetic local mismatch produces DIR/N/NAME and meta.json
           with every field, plus the --keep-dir copy.
  remote:  a stubbed remote read that never settles is a mismatch
           ("unsettled", verdict 1) with settled=false in its sidecar; one
           that settles is counted and captured with the settle time and
           version; a final-verification mismatch is captured too.
  capture: the --keep-dir copy is unchanged after the file is modified
           once the lock is released (a hard link would have followed).
W155 (docs/status/w155-oracle-correlation-20261023.md), with a fault hook
instead of a cluster:
  stages:  a modify failing at open, truncate (not applied / applied),
           the second write(2) and close (not applied / applied, the nuc b1
           shape) records the failing stage, bytes submitted, inode and
           attempted digest; the file becomes unresolved with exactly the
           syscall-boundary candidates it reached, and final verification
           classifies it base / intermediate / attempted. A create and an
           unlink that fail are recorded too.
  quarantine: modifiers and deleters leave an unresolved file alone; reads
           matching a candidate are observations, mixed bytes are a
           mismatch and foreign at final verification (saved, verdict FAIL).
  verdict: errors or unresolved files never PASS (FAIL 1, INCOMPLETE 5);
           the wrapper writes INCOMPLETE and copies the daemon logs.
  correlate: scripts/w155_correlate.py on sample client/server lines from
           the nuc b1 capture: the retry, report-loop, flush-meta, failed
           report-split, raft change, missing receipt and final version
           land in the failure's table; a legacy OUT (a.txt + meta.json)
           is anchored by inode; every logging gap is listed.
"""
import hashlib, importlib.util, json, os, pathlib, shutil, stat, subprocess, sys, tempfile, time

ROOT = pathlib.Path(__file__).resolve().parents[1]
HELPER = ROOT / "scripts/w125_p0_cluster.sh"
WRAPPER = ROOT / "scripts/w125_modify_repro.sh"
WORKLOAD = ROOT / "scripts/w125_concurrent.py"

def sh(*args, env=None, ok=None, timeout=120):
    e = dict(os.environ); e.update(env or {})
    p = subprocess.run([str(x) for x in args], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       env=e, timeout=timeout)
    if ok is not None:
        assert (p.returncode == 0) == ok, (args, p.returncode, p.stdout)
    return p

def write_exec(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)

def fake_rel(d, client_stop_rc=0):
    """A release tree with stub binaries and a client.sh whose stop exits client_stop_rc."""
    rel = d / "rel"
    (rel / "scripts").mkdir(parents=True, exist_ok=True)
    for b in ("efsd", "efs-fuse"): (rel / b).write_text(b + "\n")
    write_exec(rel / "efs-mgmt", "#!/bin/bash\necho stub-mgmt \"$@\"\n")
    write_exec(rel / "scripts/client.sh", "#!/bin/bash\n[ \"$1\" = stop ] && exit %d\nexit 0\n" % client_stop_rc)
    return rel

def run_daemon(d, i, cmd):
    """Start CMD under the helper's run-daemon wrapper as daemon nI of store d."""
    logs = d / "logs"; logs.mkdir(parents=True, exist_ok=True)
    out = open(logs / ("n%d.out" % i), "w")
    p = subprocess.Popen([str(HELPER), "run-daemon", str(logs / ("n%d.pid" % i)), str(logs / ("n%d.rc" % i)),
                          "bash", "-c", cmd], stdout=out, stderr=subprocess.STDOUT)
    for _ in range(100):
        if (logs / ("n%d.pid" % i)).exists() and (logs / ("n%d.pid" % i)).read_text().strip(): break
        time.sleep(0.05)
    else: raise AssertionError("run-daemon n%d wrote no pid" % i)
    return p

ORDERLY = 'trap "exit 0" TERM; while :; do sleep 0.1; done'
STUBBORN = 'trap "" TERM; while :; do sleep 0.1; done'

def pids_gone(d):
    for f in (d / "logs").glob("n*.pid"):
        pid = int(f.read_text())
        try: os.kill(pid, 0)
        except ProcessLookupError: continue
        return False
    return True

def test_stop(tmp):
    # 1. three orderly daemons, client stop ok -> clean, rc 0
    d = tmp / "clean"; rel = fake_rel(tmp / "clean-rel-dir")
    procs = [run_daemon(d, i, ORDERLY) for i in (1, 2, 3)]
    t0 = time.time()
    p = sh(HELPER, "stop", rel, d, env={"W125_STOP_WAIT": "5"}, ok=True)
    assert p.stdout.strip().splitlines()[-1] == "stopped clean", p.stdout
    assert time.time() - t0 < 5, "clean stop should not use the whole wait"
    assert all((d / "logs" / ("n%d.rc" % i)).read_text().strip() == "0" for i in (1, 2, 3))
    assert pids_gone(d)
    for q in procs: q.wait(timeout=10)
    # 2. one daemon ignores TERM -> needed KILL, rc 1, after the wait
    d = tmp / "kill"
    procs = [run_daemon(d, 1, ORDERLY), run_daemon(d, 2, STUBBORN), run_daemon(d, 3, ORDERLY)]
    t0 = time.time()
    p = sh(HELPER, "stop", rel, d, env={"W125_STOP_WAIT": "2"}, ok=False)
    assert p.returncode == 1 and "stopped forced" in p.stdout, p.stdout
    assert "n2 pid" in p.stdout and "needed KILL" in p.stdout, p.stdout
    assert "n1 " not in p.stdout and "n3 " not in p.stdout, p.stdout
    assert 1 <= time.time() - t0 < 15, time.time() - t0  # whole-second deadline
    assert pids_gone(d)
    assert (d / "logs/n2.rc").read_text().strip() == "137", (d / "logs/n2.rc").read_text()
    for q in procs: q.wait(timeout=10)
    # 3. a daemon that already exited non-zero is reported
    d = tmp / "crashed"
    procs = [run_daemon(d, 1, ORDERLY), run_daemon(d, 2, "exit 7"), run_daemon(d, 3, ORDERLY)]
    procs[1].wait(timeout=10)
    p = sh(HELPER, "stop", rel, d, env={"W125_STOP_WAIT": "5"}, ok=False)
    assert "n2 pid" in p.stdout and "exited rc=7" in p.stdout and "stopped forced" in p.stdout, p.stdout
    for q in procs: q.wait(timeout=10)
    # 4. a missing pid file is a forced stop too
    d = tmp / "nopid"; (d / "logs").mkdir(parents=True)
    p = sh(HELPER, "stop", rel, d, ok=False)
    assert "no pid recorded" in p.stdout and "stopped forced" in p.stdout, p.stdout
    # 5. run-daemon records the child's status and exits with it
    d = tmp / "rc"
    q = run_daemon(d, 1, "exit 3"); q.wait(timeout=10)
    assert q.returncode == 3 and (d / "logs/n1.rc").read_text().strip() == "3"
    print("stop: clean/KILL/non-zero/no-pid/run-daemon PASS")

def stub_helper(rel, stop_rc=0, stop_text="stopped clean", start_rc=0):
    """A cluster helper that creates the mount directory and ends stop with stop_text/stop_rc."""
    write_exec(rel / "scripts/w125_p0_cluster.sh", """#!/bin/bash
cmd=$1; D=$3
case $cmd in
start) mkdir -p "$D/logs" "$D/mnt/c" && echo "started stub" > "$D/logs/fuse.log"; echo "mounted $D/mnt"; exit %d ;;
stop) echo "%s"; exit %d ;;
esac
""" % (start_rc, stop_text, stop_rc))

def stub_workload(path, rc=0):
    write_exec(path, "#!/usr/bin/env python3\nimport sys\nprint('stub workload', sys.argv[1:])\nsys.exit(%d)\n" % rc)

def wrapper_env(tmp):
    # HOME without bin/edelete, so the PASS path uses rm -rf on the temporary store.
    home = tmp / "home"; home.mkdir(exist_ok=True)
    return {"HOME": str(home)}

def test_wrapper(tmp):
    stub = tmp / "stub_workload.py"; stub_workload(stub, 0)
    # a. stop fails -> forced, store kept, exit 3, done says so
    rel = fake_rel(tmp / "wa"); stub_helper(rel, stop_rc=1, stop_text="client stop rc=1\nstopped forced")
    run, out = tmp / "wa-run", tmp / "wa-out"
    p = sh(WRAPPER, rel, out, 17900, 1, 3000, env=dict(wrapper_env(tmp), W125_RUN=str(run), W125_WORKLOAD=str(stub)), ok=False)
    assert p.returncode == 3, (p.returncode, p.stdout)
    done = (out / "done").read_text()
    assert done.startswith("FAIL workload_rc=0 stop_rc=1 stop=forced store=" + str(run)), done
    assert run.is_dir() and (run / "mnt/c").is_dir(), "store must be kept after a forced stop"
    assert "stopped forced" in (out / "stop.txt").read_text()
    runtxt = (out / "run.txt").read_text()
    assert "workload_rc=0 stop_rc=1 stop=forced" in runtxt, runtxt
    assert "stop forced" in p.stdout and str(run) in p.stdout, p.stdout
    # b. stop needed KILL (same contract: non-zero) -> forced, kept
    rel = fake_rel(tmp / "wb"); stub_helper(rel, stop_rc=1, stop_text="n2 pid 1234: still running after 60s, needed KILL\nstopped forced")
    run, out = tmp / "wb-run", tmp / "wb-out"
    p = sh(WRAPPER, rel, out, 17900, 1, 3000, env=dict(wrapper_env(tmp), W125_RUN=str(run), W125_WORKLOAD=str(stub)), ok=False)
    assert p.returncode == 3 and run.is_dir() and "stop=forced" in (out / "done").read_text()
    assert "needed KILL" in (out / "stop.txt").read_text()
    # c. failing workload, clean stop -> exit 1, store kept
    bad = tmp / "stub_bad.py"; stub_workload(bad, 1)
    rel = fake_rel(tmp / "wc"); stub_helper(rel)
    run, out = tmp / "wc-run", tmp / "wc-out"
    p = sh(WRAPPER, rel, out, 17900, 1, 3000, env=dict(wrapper_env(tmp), W125_RUN=str(run), W125_WORKLOAD=str(bad)), ok=False)
    assert p.returncode == 1 and run.is_dir(), (p.returncode, p.stdout)
    assert (out / "done").read_text().startswith("FAIL workload_rc=1 stop_rc=0 stop=clean store="), (out / "done").read_text()
    # d. a reused OUT is refused without touching the first run's artifacts
    before = {f.name: f.read_text() for f in out.iterdir() if f.is_file()}
    p = sh(WRAPPER, rel, out, 17900, 1, 3000, env=dict(wrapper_env(tmp), W125_RUN=str(tmp / "wd-run"), W125_WORKLOAD=str(bad)), ok=False)
    assert p.returncode == 2 and "refusing to reuse" in p.stdout, p.stdout
    assert {f.name: f.read_text() for f in out.iterdir() if f.is_file()} == before
    assert not (tmp / "wd-run").exists()
    # e. start fails -> exit 2, store kept, stop status recorded
    rel = fake_rel(tmp / "we"); stub_helper(rel, start_rc=1, stop_rc=1, stop_text="stopped forced")
    run, out = tmp / "we-run", tmp / "we-out"
    p = sh(WRAPPER, rel, out, 17900, 1, 3000, env=dict(wrapper_env(tmp), W125_RUN=str(run), W125_WORKLOAD=str(stub)), ok=False)
    assert p.returncode == 2 and (out / "done").read_text().startswith("FAIL start failed stop_rc=1 store="), (out / "done").read_text()
    # f. the real workload for one second, clean stop -> PASS, store deleted
    rel = fake_rel(tmp / "wf"); stub_helper(rel)
    run, out = tmp / "wf-run", tmp / "wf-out"
    p = sh(WRAPPER, rel, out, 17900, 1, 3000, env=dict(wrapper_env(tmp), W125_RUN=str(run)), ok=True, timeout=300)
    assert (out / "done").read_text().strip() == "PASS stop=clean store deleted", (out / "done").read_text()
    assert not run.exists(), "a passing run's store is deleted after a clean stop"
    a = (out / "a.txt").read_text()
    assert "final live=" in a and "RC=0" in a and "mismatch=0" in a, a
    assert "delete_rc=0" in (out / "run.txt").read_text()
    assert not (out / "mismatch").exists()
    assert "verdict=PASS" in a and "unresolved=0" in a, a
    assert (out / "attempts.jsonl").read_text() == "", "a clean run records no failed attempt"
    print("wrapper: forced stop (fail/KILL) keeps store, failing workload, reused OUT, start failure, PASS deletes PASS")

def load_workload():
    spec = importlib.util.spec_from_file_location("w125_concurrent", WORKLOAD)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m

SIDECAR = {"name", "source", "locked", "read_time", "save_time", "host", "argv", "root", "capture", "version",
           "expected_len", "expected_sha256", "actual_len", "actual_sha256", "first_diff", "ino",
           "kept", "kept_len", "kept_sha256", "reread_len", "reread_sha256", "reread_same_as_read"}
REMOTE_EXTRA = {"candidates", "nearest_version", "settled", "settle_time", "settle_after_s", "retries"}

def meta_of(mm, n): return json.loads((mm / str(n) / "meta.json").read_text())
def sha(b): return hashlib.sha256(b).hexdigest()

def test_sidecar_and_capture(tmp):
    m = load_workload()
    c, mm, keep = tmp / "s/c", tmp / "s/mm", tmp / "s/keep"
    m.configure([str(c), "0", "--mismatch-dir", str(mm), "--keep-dir", str(keep)])
    name = "w0/d000/f000001"; want = m.contents(name, 0)
    got = bytearray(want); got[5] ^= 0xff; got = bytes(got)
    f = c / name; f.parent.mkdir(parents=True); f.write_bytes(got)
    l = m.flock(name); l.acquire()
    try: n, d, meta = m.save_mismatch(name, got, 0, want, source="local")
    finally: l.release()
    assert n == 1 and pathlib.Path(d) == mm / "1"
    assert SIDECAR <= set(meta), SIDECAR - set(meta)
    assert "artifact_error" not in meta, meta
    disk = meta_of(mm, 1)
    assert disk == json.loads(json.dumps(meta))
    assert disk["source"] == "local" and disk["locked"] is True and disk["version"] == 0
    assert disk["first_diff"] == 5 and disk["expected_len"] == len(want) == disk["actual_len"]
    assert disk["expected_sha256"] == sha(want) and disk["actual_sha256"] == sha(got)
    assert disk["ino"] == f.stat().st_ino and disk["read_time"] <= disk["save_time"]
    assert (mm / "1" / name).read_bytes() == got
    assert disk["reread_same_as_read"] is True and disk["kept_sha256"] == sha(got) == disk["reread_sha256"]
    kept = pathlib.Path(disk["kept"])
    assert kept == keep / "1" and kept.read_bytes() == got
    assert kept.stat().st_nlink == 1, "the capture is a copy, not a hard link"
    # a modifier changes the file after the lock is released: the capture stays
    f.write_bytes(m.contents(name, 1))
    assert kept.read_bytes() == got and sha(kept.read_bytes()) == disk["kept_sha256"]
    assert f.read_bytes() != got
    # the second capture of the same name gets its own number; a stale copy at
    # KEEP/N is an artifact error, never silently overwritten
    (keep / "2").write_bytes(b"stale")
    n2, d2, meta2 = m.save_mismatch(name, got, 1, m.contents(name, 1), source="local")
    assert n2 == 2 and "artifact_error" in meta2 and m.st["artifact_errors"] == 1, meta2
    assert (keep / "2").read_bytes() == b"stale" and (mm / "2" / name).read_bytes() == got
    assert m.st["saved"] == 2
    # the saved count is capped per source
    for _ in range(m.SAVE_CAP): m.save_mismatch(name, got, 0, want, source="local")
    assert m.saved_by["local"] == m.SAVE_CAP and not (mm / str(m.SAVE_CAP + 1)).exists()
    assert m.save_mismatch(name, got, 0, want, source="final") is not None, "other sources have their own cap"
    print("sidecar: every field, capture unchanged after modification, O_EXCL, per-source cap PASS")

def test_remote(tmp):
    m = load_workload()
    c, mm, keep = tmp / "r/c", tmp / "r/mm", tmp / "r/keep"
    m.configure([str(c), "0", "--remote", "--mismatch-dir", str(mm), "--keep-dir", str(keep)])
    m.RETRY_WAIT_S = 0.01
    name = "w1/d000/f000002"; v2 = m.contents(name, 2)
    f = c / name; f.parent.mkdir(parents=True); f.write_bytes(v2)
    torn = v2[:len(v2) // 2] + b"\0" * (len(v2) - len(v2) // 2)
    # never settles -> unsettled mismatch, sidecar says so, verdict 1
    m.read_file = lambda n: torn
    assert m.remote_check(name) == "unsettled"
    assert m.st["mismatch"] == 1 and m.st["unsettled"] == 1 and m.st["settled"] == 0 and m.st["read"] == 0
    assert m.verdict(0) == 1
    meta = meta_of(mm, 1)
    assert (SIDECAR | REMOTE_EXTRA) <= set(meta), (SIDECAR | REMOTE_EXTRA) - set(meta)
    assert meta["source"] == "remote" and meta["locked"] is False and meta["settled"] is False
    assert meta["version"] is None and meta["retries"] == m.RETRIES and meta["settle_after_s"] >= 0
    assert len(meta["candidates"]) == m.MAX_VERSIONS + 1 and meta["candidates"][2]["sha256"] == sha(v2)
    assert meta["actual_sha256"] == sha(torn) and (mm / "1" / name).read_bytes() == torn
    assert meta["kept_sha256"] == sha(v2), "the keep copy re-reads the file itself"
    assert any("unsettled" in e for e in m.errs), m.errs
    # settles on the second re-read -> counted, captured, not a mismatch
    seq = iter([torn, torn, v2]); m.read_file = lambda n: next(seq)
    assert m.remote_check(name) == "settled"
    assert m.st["settled"] == 1 and m.st["mismatch"] == 1 and m.st["read"] == 1 and m.st["newer"] == 1
    meta = meta_of(mm, 2)
    assert meta["settled"] is True and meta["version"] == 2 and meta["retries"] == 2
    assert meta["expected_sha256"] == sha(v2) and meta["expected_len"] == len(v2)
    assert meta["settle_after_s"] >= 2 * m.RETRY_WAIT_S and meta["settle_time"] > meta["read_time"]
    assert meta["first_diff"] == len(v2) // 2 and (mm / "2" / name).read_bytes() == torn
    # a matching first read saves nothing
    m.read_file = lambda n: v2
    assert m.remote_check(name) == "ok" and m.st["saved"] == 2
    # a file that disappears while unsettled is ENOENT, not a mismatch, and the sidecar closes unsettled
    def vanish(n): raise FileNotFoundError(n)
    seq2 = iter([torn]); m.read_file = lambda n: next(seq2, None) or vanish(n)
    assert m.remote_check(name) == "enoent" and m.st["mismatch"] == 1
    assert meta_of(mm, 3)["settled"] is False and meta_of(mm, 3)["retries"] == 1
    # final verification (remote: any version) captures a mismatch as source "final"
    m.read_file = lambda n: open(os.path.join(m.a.root, n), "rb").read()
    m.live.append(name); f.write_bytes(torn)
    bad, missing = m.final_verify()
    assert bad == 1 and not missing and m.verdict(bad) == 1
    meta = meta_of(mm, 4)
    assert meta["source"] == "final" and meta["locked"] is False and meta["version"] is None
    assert (mm / "4" / name).read_bytes() == torn and any(e.startswith("final mismatch") for e in m.errs)
    # final verification on the local client: expected version from the lock-protected table
    m2 = load_workload()
    c2, mm2 = tmp / "f/c", tmp / "f/mm"
    m2.configure([str(c2), "0", "--mismatch-dir", str(mm2)])
    f2 = c2 / name; f2.parent.mkdir(parents=True); f2.write_bytes(torn)
    m2.live.append(name); m2.ver[name] = 2
    bad, missing = m2.final_verify()
    assert bad == 1 and m2.verdict(bad) == 1
    meta = meta_of(mm2, 1)
    assert meta["source"] == "final" and meta["locked"] is True and meta["version"] == 2
    assert meta["expected_sha256"] == sha(v2) and meta["actual_sha256"] == sha(torn) and "kept" not in meta
    assert m2.st["saved"] == 1
    print("remote: unsettled fails and is captured, settled is counted and captured, final mismatches captured PASS")

def test_inspect_still_reads_model(tmp):
    m = load_workload()
    c = tmp / "i/c"; name = "w0/d001/f000003"
    f = c / name; f.parent.mkdir(parents=True); f.write_bytes(m.contents(name, 3))
    p = sh(sys.executable, ROOT / "scripts/w125_modify_inspect.py", c, name, ok=True)
    assert "exact=[3]" in p.stdout, p.stdout
    print("inspect: content model still importable PASS")

import errno as _errno

def find_name(m, op, minlen=0, base_min=0):
    """The first writer name whose v1 operation is OP with at least MINLEN bytes."""
    for i in range(100000):
        name = "w0/d%03d/f%06d" % (i % 50, i)
        o = m.op_of(name, 1)
        if o[0] == op and (op != "rewrite" or len(o[1]) >= minlen) and len(m.contents(name, 0)) >= base_min:
            return name
    raise AssertionError("no %s name" % op)

def eio_at(stage_name, nth=1, apply=False):
    """A fault hook: the NTH call of STAGE_NAME raises EIO, after running it when APPLY."""
    seen = {}
    def hook(rec, st, fn):
        seen[st] = seen.get(st, 0) + 1
        if st == stage_name and seen[st] == nth:
            if apply: fn()
            raise OSError(_errno.EIO, "Input/output error")
        return fn()
    return hook

def w155_setup(tmp, tag, name, extra=()):
    m = load_workload()
    c, mm = tmp / tag / "c", tmp / tag / "mm"
    m.configure([str(c), "0", "--mismatch-dir", str(mm), "--modify-log", str(tmp / tag / "attempts.jsonl")] + list(extra))
    f = c / name; f.parent.mkdir(parents=True); f.write_bytes(m.contents(name, 0))
    m.live.append(name)
    return m, f, mm

def one_modify(m):
    """One modifier pass: stop as soon as the single live file was attempted."""
    import threading
    name = m.live[0]
    t = threading.Thread(target=m.modifier, args=(0,)); t.start()
    for _ in range(500):
        if m.ver.get(name, 0) or name in m.unresolved: break
        time.sleep(0.01)
    m.stop.set(); t.join(timeout=10); m.stop.clear()

def test_w155_stages(tmp):
    cases = [  # (tag, op, stage, nth, apply, failed_stage, bytes_submitted(chunks), candidate labels, final class)
        ("open", "rewrite", "open", 1, False, "open", 0, ["base v0"], "base"),
        ("trunc", "rewrite", "truncate", 1, False, "truncate", 0, ["base v0", "truncate 0 (failing call)"], "base"),
        ("trunc-applied", "rewrite", "truncate", 1, True, "truncate", 0, ["base v0", "truncate 0 (failing call)"],
         "intermediate:truncate 0 (failing call)"),
        ("write2", "rewrite", "write", 2, False, "write", 1, None, "intermediate:write"),
        ("close", "rewrite", "close", 1, False, "close", None, None, "attempted"),
        ("close-applied", "rewrite", "close", 1, True, "close", None, None, "attempted"),
        ("shrink", "shrink", "truncate", 1, False, "truncate", 0, None, "base"),
    ]
    for tag, op, st, nth, apply, failed, chunks, labels, final in cases:
        m0 = load_workload()
        name = find_name(m0, op, minlen=int(2.5 * m0.WRITE_CHUNK), base_min=1000)
        m, f, mm = w155_setup(tmp / "w155", tag, name)
        m.fault = eio_at(st, nth, apply)     # close not applied: the bytes written are visible anyway
        one_modify(m)
        m.fault = None
        assert name in m.unresolved, (tag, m.errs)
        u = m.unresolved[name]
        assert u["failed_stage"] == failed and u["errno"] == _errno.EIO, (tag, u["failed_stage"], u["errno"])
        assert u["ino"] == (f.stat().st_ino if failed != "open" else None), (tag, u["ino"])
        new = m.contents(name, 1)
        assert u["attempted_sha256"] == sha(new) and u["attempted_len"] == len(new)
        assert u["base_sha256"] == sha(m.contents(name, 0)) and u["version"] == 1 and u["base_version"] == 0
        fs = [x for x in u["stages"] if x["errno"]]
        assert fs[0]["stage"] == failed and fs[0]["t0"] <= fs[0]["t1"] and u["t_fail"] == fs[0]["t0"], (tag, u["stages"])
        if chunks is not None and op == "rewrite":
            assert u["bytes_submitted"] == chunks * m.WRITE_CHUNK, (tag, u["bytes_submitted"])
        if failed == "close":
            assert u["bytes_submitted"] == u["bytes_total"] == len(new), (tag, u["bytes_submitted"])
        labs = [c["label"] for c in u["candidates"]]
        if labels is not None: assert labs == labels, (tag, labs)
        if tag == "write2":
            assert labs == ["base v0", "truncate 0", "write %d/%d" % (m.WRITE_CHUNK, len(new)),
                            "write %d+%d/%d (failing call)" % (m.WRITE_CHUNK, m.WRITE_CHUNK, len(new))], labs
        if failed == "close":
            assert labs[-1].endswith("= attempted v1") and labs[:2] == ["base v0", "truncate 0"], labs
            assert all(c["label"] != labs[-1] or c["sha256"] == sha(new) for c in u["candidates"])
        assert m.ver.get(name, 0) == 0, "a failed modify never advances the known version"
        assert m.st["unresolved"] == 1 and m.st["errors"] == 1 and m.st["modified"] == 0
        assert any(("stage=%s" % failed) in e and "ino=" in e and "@20" in e for e in m.errs), m.errs
        bad, missing = m.final_verify()
        assert bad == 0 and not missing, (tag, bad, missing)
        cls = u["final"]["classification"]
        assert cls == final or (final.endswith(":write") and cls.startswith("intermediate:write ")), (tag, cls)
        assert m.verdict(bad) == 1, "an EIO modify is a FAIL, whatever the file holds"
        recs = [json.loads(l) for l in (tmp / "w155" / tag / "attempts.jsonl").read_text().splitlines()]
        assert [r["kind"] for r in recs] == ["modify", "final-classification"], recs
        assert recs[0]["failed_stage"] == failed and recs[0]["candidates"] == u["candidates"] and "cands" not in recs[0]
        assert recs[1]["classification"] == cls and recs[1]["name"] == name
        if tag == "close-applied": big = (m, name, f, u)
    # a create that fails at rename and an unlink that fails are recorded with stage and time
    m = load_workload(); c = tmp / "w155/cw/c"
    m.configure([str(c), "0", "--modify-log", str(tmp / "w155/cw/attempts.jsonl")])
    m.fault = eio_at("rename")
    import threading
    t = threading.Thread(target=m.writer, args=(0,)); t.start(); time.sleep(0.2); m.stop.set(); t.join(10)
    m.fault = None
    cr = [r for r in m.attempts if r["kind"] == "create"]
    assert cr and cr[0]["failed_stage"] == "rename" and cr[0]["ino"] and cr[0]["bytes_submitted"] == cr[0]["attempted_len"]
    assert cr[0]["name"] not in m.live, "an unpublished create is not modelled"
    print("w155 stages: open/truncate/write/close failures recorded, candidates per boundary, classified PASS")
    return big

def test_w155_quarantine(tmp):
    import threading
    m0 = load_workload(); name = find_name(m0, "rewrite", minlen=int(2.5 * m0.WRITE_CHUNK), base_min=1000)
    m, f, mm = w155_setup(tmp / "w155q", "q", name)
    m.fault = eio_at("close", apply=True); one_modify(m); m.fault = None
    u = m.unresolved[name]; new = m.contents(name, 1)
    assert f.read_bytes() == new, "the applied-then-EIO close left the attempted version visible"
    # quarantine: a modifier never attempts it again; a deleter never deletes it
    t = threading.Thread(target=m.modifier, args=(0,)); t.start(); time.sleep(0.3); m.stop.set(); t.join(10); m.stop.clear()
    assert len(m.attempts) == 1 and m.ver.get(name, 0) == 0 and f.read_bytes() == new
    others = []
    for i in range(30):
        o = "w9/d000/f%06d" % i; p = m.a.root + "/" + o
        os.makedirs(os.path.dirname(p), exist_ok=True); open(p, "wb").write(m.body(o)); others.append(o)
    m.live.extend(others); m.st["written"] = 1000
    t = threading.Thread(target=m.deleter, args=(0,)); t.start()
    for _ in range(300):
        if m.st["deleted"] >= 15: break
        time.sleep(0.01)
    m.stop.set(); t.join(10); m.stop.clear()
    assert m.st["deleted"] >= 10 and name in m.live and f.exists(), (m.st["deleted"], name in m.live)
    # reads: a candidate is an observation, not a mismatch; a mix is a mismatch with the candidates saved
    assert m.check_unresolved(name, new, time.time()) and m.check_unresolved(name, m.contents(name, 0), time.time())
    assert m.st["mismatch"] == 0 and m.st["unresolved_reads"] == 2
    assert set(u["observed"]) == {u["candidates"][0]["label"], u["candidates"][-1]["label"]}, u["observed"]
    old = m.contents(name, 0)
    mixed = new[:m.WRITE_CHUNK // 2] + old[m.WRITE_CHUNK // 2:] if len(old) > m.WRITE_CHUNK else old + new[len(old):]
    assert mixed not in [c for _, c in u["cands"]]
    assert not m.check_unresolved(name, mixed, time.time())
    assert m.st["mismatch"] == 1 and m.verdict(0) == 1
    meta = meta_of(mm, 1)
    assert meta["unresolved"] is True and meta["version"] is None and [c["label"] for c in meta["candidates"]] == \
        [c["label"] for c in u["candidates"]] and meta["actual_sha256"] == sha(mixed)
    # the reader thread routes an unresolved file through its candidates
    f.write_bytes(new); r0 = m.st["unresolved_reads"]
    t = threading.Thread(target=m.reader, args=(0,)); t.start()
    for _ in range(300):
        if m.st["unresolved_reads"] > r0: break
        time.sleep(0.01)
    m.stop.set(); t.join(10); m.stop.clear()
    assert m.st["unresolved_reads"] > r0 and m.st["mismatch"] == 1, (m.st["unresolved_reads"], m.st["mismatch"])
    f.write_bytes(mixed)
    bad, missing = m.final_verify()
    assert bad == 1 and u["final"]["classification"] == "foreign" and m.st["cls_foreign"] == 1
    assert meta_of(mm, 2)["source"] == "final" and meta_of(mm, 2)["unresolved"] is True
    assert any(e.startswith("final unresolved") for e in m.errs)
    # verdict: unresolved without any counted error is INCOMPLETE, never PASS
    m2 = load_workload(); m2.configure([str(tmp / "w155q/v"), "0"])
    assert m2.verdict(0) == 0 and m2.verdict_name(0) == "PASS"
    m2.st["unresolved"] = 1
    assert m2.verdict(0) == m2.INCOMPLETE_RC == 5 and m2.verdict_name(5) == "INCOMPLETE"
    m2.st["errors"] = 1
    assert m2.verdict(0) == 1
    m2.st.update(errors=0, unresolved=0, cls_missing=1)
    assert m2.verdict(0) == 5
    print("w155 quarantine: no modify/delete of an unresolved file, candidates accepted, mix is a mismatch, "
          "foreign at final, INCOMPLETE/FAIL never PASS")

def test_w155_main(tmp):
    """main() end to end: one modify fails at close (applied), the run is FAIL with the file listed."""
    import contextlib, io
    m = load_workload()
    def hook(rec, st, fn):
        if rec["kind"] == "modify" and st == "close" and not hook.done:
            hook.done = True; fn(); raise OSError(_errno.EIO, "Input/output error")
        return fn()
    hook.done = False; m.fault = hook
    log = tmp / "w155m/attempts.jsonl"; log.parent.mkdir(parents=True)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = m.main([str(tmp / "w155m/c"), "1", "--writers", "1", "--readers", "1", "--deleters", "0",
                     "--modifiers", "1", "--modify-log", str(log)])
    o = buf.getvalue()
    assert hook.done and rc == 1 and "verdict=FAIL" in o and "unresolved=1 " in o, o
    assert "classified base=0 attempted=1 " in o and "UNRESOLVED " in o and "stage=close errno=5" in o, o
    assert "final=attempted (" in o and "ERR errors modify " in o, o
    kinds = [json.loads(l)["kind"] for l in log.read_text().splitlines()]
    assert kinds == ["modify", "final-classification"], kinds
    print("w155 main: applied-then-EIO close -> verdict=FAIL, UNRESOLVED line, classified attempted PASS")

def test_w155_wrapper(tmp):
    stub = tmp / "stub_incomplete.py"; stub_workload(stub, 5)
    rel = fake_rel(tmp / "wi"); stub_helper(rel)
    h = rel / "scripts/w125_p0_cluster.sh"
    h.write_text(h.read_text().replace('echo "started stub" > "$D/logs/fuse.log";',
                                       'echo "started stub" > "$D/logs/fuse.log"; echo n1 > "$D/logs/efsd-n1.log";'))
    run, out = tmp / "wi-run", tmp / "wi-out"
    p = sh(WRAPPER, rel, out, 17900, 1, 3000, env=dict(wrapper_env(tmp), W125_RUN=str(run), W125_WORKLOAD=str(stub)), ok=False)
    assert p.returncode == 5, (p.returncode, p.stdout)
    assert (out / "done").read_text().startswith("INCOMPLETE workload_rc=5 stop_rc=0 stop=clean store="), (out / "done").read_text()
    assert run.is_dir() and (out / "efsd-n1.log").read_text() == "n1\n"
    assert "--modify-log" in (out / "a.txt").read_text() and str(out / "attempts.jsonl") in (out / "a.txt").read_text()
    print("w155 wrapper: INCOMPLETE recorded, store kept, daemon logs copied, --modify-log passed PASS")

# Verbatim lines from the nuc W148 rerun, attempt b1 (fuse.log and efsd-n1/n2.log,
# 2026-10-23 04:03:2x-04:04:0x UTC); times are rebased onto the synthetic failure.
B1_CLIENT = [
    (-2.0, "opid-admission: BUSY oldest=38401 next=38417 live=1 window=16"),
    (0.0, "inode-rpc: retry type=67 attempt=0 why=recv rc=-6 req_len=8892 recv_ms=30012"),
    (0.0, "report-loop ino={ino} rounds=1 stale=0 busy=1 ms=3099 rpc_ms=3099 replay_ms=0 sleep_ms=0 recs=25 rc=-13"),
    (0.0, "efs-fuse flush-meta: resource busy (efs_rc=-13) ino={ino} off=0 len=0"),
    (0.04, "efs: rename_at 2438/f000535.tmp -> 2438/f000535 rc=-13 after 2 tries"),
    (0.136, "inode-rpc: retry type=87 attempt=0 why=recv rc=-6"),
    (2.0, "inode-rpc: slow-ok type=87 attempts=1 saw_busy=0 status=0 us=1261517"),
    (300.0, "inode-rpc: retry type=67 attempt=0 why=recv rc=-6 req_len=100 recv_ms=30000"),
]
B1_N1 = [
    (-25.0, "report-split nrec=12 pack_ms=0 push_ms=0 finish_ms=119 skip=0 stale=0 inval=0 rc=0 fail=-/0"),
    (-20.0, "raft: group=0 prevote refused for=1 term=2 cand_last=7289/1 my_last=7289/1 my_term=1 leader=-1"),
    (1.3, "report-split nrec=11 pack_ms=0 push_ms=0 finish_ms=150 skip=0 stale=0 inval=0 rc=-14 fail=finish/-14"),
    (1.5, "raft-host: txn-recover shard=3 coord=2 parts=2 age=31.0s -> COMMIT (resolved)"),
    (2.0, "kv-flush: files=139 io_ms=3155 manifest_ms=91 view_yielded=1 rc=0"),
]
B1_N2 = [(-29.0, "raft: group=0 became leader term=2 noop=7290 commit=7289 applied=7289 events=3 skipped=1"),
         (-10.0, "raft-host: apply-sleep x"), (1.0, "raft-host: apply-sleep y")]

# The same failure as a W155 build logs it: the retry names node and request
# id, report-loop the publication id, the client resolves by identity and the
# server answers the receipt query (synthetic ids, b1 shape).
W155_ID = "55150000/7/1a2b:9"
W155_CLIENT = [
    (0.0, "inode-rpc: retry type=67 attempt=0 why=recv rc=-6 req_len=8892 recv_ms=30012 nid=1 id=" + W155_ID),
    (0.0, "report-loop ino={ino} rounds=1 stale=0 busy=1 ms=3099 rpc_ms=3099 replay_ms=0 sleep_ms=0 recs=25 "
          "rc=-13 pub=1a2b:9 sent=1"),
    (0.0, "report-resolve id=" + W155_ID + " ino={ino} recs=25 rc=-13 sent=1: outcome not known; same-identity "
          "lookup for 8000 ms"),
    (0.1, "report-resolve id=" + W155_ID + " ino={ino} outcome=applied rc=0 first_rc=-13 from=1 queries=1 "
          "resends=0 none=0/1 ms=3"),
    (0.2, "flush-meta stage=report ino={ino} report_rc=-1 rc=-1 stalled=0"),
]
W155_N1 = [
    (-29.0, "report-split nrec=25 pack_ms=0 push_ms=0 finish_ms=119 skip=0 stale=0 inval=0 rc=0 fail=-/0 "
            "ino={ino} pub=1a2b:9"),
    (-29.0, "report-receipt id=" + W155_ID + " ino={ino} nrec=25 how=ran rc=0 outcome=applied attempts=1 "
            "joined=0 replayed=0 phase=- ms=30200"),
    (0.05, "report-receipt query id=" + W155_ID + " state=2 outcome=applied rc=0 attempts=1 age_ms=29000"),
]

def write_log(path, base, lines, ino):
    with open(path, "w") as f:
        for dt, text in lines:
            t = base + dt
            f.write("%s %s\n" % (time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(t)) + ".%03dZ" % int(t % 1 * 1000),
                                 text.format(ino=ino)))

def test_w155_correlate(tmp, big):
    m, name, f, u = big
    out = tmp / "corr-out"; out.mkdir()
    # attempts.jsonl as the harness wrote it (the close-applied case), with its final classification
    shutil.copy(pathlib.Path(m.a.modify_log), out / "attempts.jsonl")
    fs = [x for x in u["stages"] if x["errno"]][0]
    end = fs["t1"]
    write_log(out / "fuse.log", end, B1_CLIENT, u["ino"])
    write_log(out / "efsd-n1.log", end, B1_N1, u["ino"])
    write_log(out / "efsd-n2.log", end, B1_N2, u["ino"])
    js = tmp / "corr.json"
    p = sh(sys.executable, ROOT / "scripts/w155_correlate.py", out, "--json", js, ok=True)
    res = json.loads(js.read_text())
    assert len(res) == 1, res
    r = res[0]
    assert r["failure"]["name"] == name and r["failure"]["failed_stage"] == "close" and r["anchor"] == "failing stage close"
    rt = r["client"]["retries"]
    assert [(x["type"], x["type_name"], x["why"], x["recv_ms"]) for x in rt] == \
        [(67, "REPORT_CHUNKS", "recv", 30012), (87, "INODE_RENAME_AT", "recv", None)], rt
    assert [x["ino"] for x in r["client"]["report_loop"]] == [u["ino"]] and r["client"]["report_loop"][0]["rc"] == "-13"
    assert [(x["where"], x["efs_rc"], x["ino"]) for x in r["client"]["fuse_errors"]] == [("flush-meta", -13, u["ino"])]
    assert r["client"]["admission_busy"][0]["oldest"] == "38401" and r["client"]["rename_at"][0]["rc"] == -13
    n1, n2 = r["servers"]["n1"], r["servers"]["n2"]
    assert [x["fail"] for x in n1["report_split_failed"]] == ["finish/-14"] and n1["report_split"] == 1
    assert len(n1["txn_recover"]) == 1 and n1["kv_flush"][0]["io_ms"] == "3155" and n1["raft"] == []
    assert n2["apply_sleep"] == 1 and n2["raft"] == [] and n2["silence_s"] >= 10
    assert r["receipt"] == [] and "not logged" in p.stdout and "(gap G2)" in p.stdout
    assert r["failure"]["final"]["classification"] == "attempted" and "final observed" in p.stdout
    for g in ("G1", "G2", "G3", "G4", "G5", "G6", "G7", "G8"): assert "- %s " % g in p.stdout, g
    assert "REPORT_CHUNKS" in p.stdout and "sent~" in p.stdout and "## failure 1: modify " + name in p.stdout
    # legacy OUT (pre-W155): a.txt ERR line, the inode from meta.json, the capture exactly v1
    lo = tmp / "corr-legacy"; (lo / "mismatch/1" / os.path.dirname(name)).mkdir(parents=True)
    new = m.contents(name, 1)
    (lo / "mismatch/1" / name).write_bytes(new)
    (lo / "mismatch/1/meta.json").write_text(json.dumps(dict(name=name, ino=u["ino"], source="local", version=0,
        read_time=end + 50, actual_len=len(new), actual_sha256=sha(new), capture="/elsewhere/" + name)))
    (lo / "a.txt").write_text("final live=1\nERR errors modify %s v1 rewrite: [Errno 5] Input/output error\n"
                              "ERR errors unlink w1/d048/f000148: [Errno 16] Device or resource busy: '/x'\n" % name)
    write_log(lo / "fuse.log", end, B1_CLIENT, u["ino"])
    js2 = tmp / "corr2.json"
    p = sh(sys.executable, ROOT / "scripts/w155_correlate.py", lo, "--json", js2, ok=True)
    res = json.loads(js2.read_text())
    assert [x["failure"]["kind"] for x in res] == ["modify", "unlink"], res
    assert res[0]["anchor"].startswith("client lines naming ino=%d" % u["ino"]) and res[0]["client"]["retries"][0]["type"] == 67
    assert res[0]["failure"]["final"]["captures"][0]["exact_version"] == 1 and "exactly v1" in p.stdout
    assert res[1]["window"] is None and "1 failure(s) here could not be anchored" in p.stdout
    assert "legacy OUT" in p.stdout and r["servers"] and res[0]["servers"] == {}
    # W155 build: identities join the retry, report-loop, resolve and the server receipts exactly
    wo = tmp / "corr-w155"; wo.mkdir()
    shutil.copy(pathlib.Path(m.a.modify_log), wo / "attempts.jsonl")
    write_log(wo / "fuse.log", end, W155_CLIENT, u["ino"])
    write_log(wo / "efsd-n1.log", end, W155_N1, u["ino"])
    js3 = tmp / "corr3.json"
    p = sh(sys.executable, ROOT / "scripts/w155_correlate.py", wo, "--json", js3, ok=True)
    r3 = json.loads(js3.read_text())[0]
    assert r3["client"]["retries"][0]["id"] == W155_ID and r3["client"]["retries"][0]["nid"] == "1"
    assert [e["id"] for e in r3["identity"]] == [W155_ID], r3["identity"]
    e = r3["identity"][0]
    assert len(e["servers"]) == 3 and len(e["client"]) == 3, e  # split+receipt+query; loop+2 resolve
    assert any("outcome=applied" in x["text"] for x in e["client"]) and r3["receipt"]
    assert "request " + W155_ID in p.stdout and "(closed by W155)" in p.stdout and "not logged" not in p.stdout
    assert r3["client"]["flush_stage"][0]["stage"] == "report" and "flush stage" in p.stdout
    for g in ("G1", "G2", "G3", "G4", "G5", "G6", "G7", "G8"): assert "- %s " % g in p.stdout, g
    print("w155 correlate: retry/report-loop/flush-meta/report-split/txn-recover/silence/receipt gap/final version "
          "tabled; legacy OUT anchored by inode; W155 identities joined to receipts; gaps G1-G8 listed PASS")

def main():
    with tempfile.TemporaryDirectory(prefix="w125-harness-") as t:
        tmp = pathlib.Path(t)
        test_stop(tmp)
        test_wrapper(tmp)
        test_sidecar_and_capture(tmp)
        test_remote(tmp)
        test_inspect_still_reads_model(tmp)
        big = test_w155_stages(tmp)
        test_w155_quarantine(tmp)
        test_w155_main(tmp)
        test_w155_wrapper(tmp)
        test_w155_correlate(tmp, big)
    print("w125 harness self-tests PASS")

if __name__ == "__main__":
    main()
