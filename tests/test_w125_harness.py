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

def main():
    with tempfile.TemporaryDirectory(prefix="w125-harness-") as t:
        tmp = pathlib.Path(t)
        test_stop(tmp)
        test_wrapper(tmp)
        test_sidecar_and_capture(tmp)
        test_remote(tmp)
        test_inspect_still_reads_model(tmp)
    print("w125 harness self-tests PASS")

if __name__ == "__main__":
    main()
