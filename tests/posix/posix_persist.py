#!/usr/bin/env python3
"""Durability suite: does what run 1 wrote survive an unmount + remount?

Every other suite here writes and verifies inside one mount session, so all of
them would still pass if efs kept everything in the client's caches and never
made it durable. This one splits each test across two processes with the FUSE
mount torn down in between:

    posix_persist.py <mount-dir> --phase prepare     # writes
    <unmount, remount>
    posix_persist.py <mount-dir> --phase verify      # reads back

A test is one function called twice, once per phase, so the expectation lives
next to the thing that produced it instead of in a manifest that can drift:

    @persist
    def small_file(d, phase):
        want = rand_bytes("small_file", 4096)
        if phase == "prepare":
            wr(os.path.join(d, "f"), want)
        else:
            eq(rd(os.path.join(d, "f")), want, "content")

Nothing is carried between the phases except the mount itself. Content is
derived from the test name via rand_bytes, so verify recomputes what prepare
should have written rather than trusting a file the same run wrote.

Unlike posix_suite.py there is no XFS baseline. "Did my data survive a
remount" has an absolute right answer, so these tests are self-validating.

Usage:
    posix_persist.py <mount-dir> --phase prepare|verify
                     [--results <file>] [--tag <id>] [--keep]
                     [--filter <substr>] [--timeout-s <sec>]

Exit code: 0 if every selected test passed.
"""
import errno
import hashlib
import os
import shutil
import signal
import tempfile
import stat as statmod
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# Reuse the suite's helpers rather than copying them. rand_bytes in particular
# must be the SAME generator: a divergent copy would silently compare a file
# against bytes it was never written with.
from posix_suite import (  # noqa: E402
    Fail, _kill_pgid, _run_pool, _write_status, eq, format_result_line,
    rand_bytes, rd, sh, wr)

CHUNK = 128 * 1024

TESTS = []


def persist(fn):
    """Register a durability test. fn(dir, phase) runs once per phase."""
    TESTS.append((fn.__name__, fn))
    return fn


def sha(b):
    return hashlib.sha256(b).hexdigest()


def eq_bytes(got, want, what="content"):
    """Compare file contents, reporting a summary instead of the bytes.

    These payloads run to megabytes of binary; the suite's generic eq() would
    put the whole repr in the failure detail and the TSV line with it. What
    actually helps triage is how the data differs -- short, long, or the same
    length but diverging at some offset, and whether the tail is zeros (the
    signature of a lost chunk rather than corruption).
    """
    if got == want:
        return
    if len(got) != len(want):
        n = min(len(got), len(want))
        pre = "first %d bytes match" % n if got[:n] == want[:n] else "and differs"
        raise Fail("%s: size got %d, want %d (%s)" % (what, len(got), len(want), pre))
    off = next(i for i in range(len(got)) if got[i] != want[i])
    tail_zero = got[off:] == b"\0" * (len(got) - off)
    raise Fail("%s: size ok (%d) but differs at offset %d%s" %
               (what, len(got), off, "; rest is zeros" if tail_zero else ""))


# --------------------------------------------------------------------------
# Content
# --------------------------------------------------------------------------

@persist
def small_file(d, phase):
    """One sub-chunk file: the simplest thing that can be lost."""
    p = os.path.join(d, "f")
    want = rand_bytes("small_file", 4096)
    if phase == "prepare":
        wr(p, want)
    else:
        eq_bytes(rd(p), want, "content")


@persist
def closed_but_not_fsynced(d, phase):
    """Written and closed, never fsynced.

    POSIX does not promise this survives a crash, but a clean unmount must
    flush it -- losing it would mean every ordinary program that writes a file
    and exits loses data.
    """
    p = os.path.join(d, "f")
    want = rand_bytes("closed_but_not_fsynced", 8192)
    if phase == "prepare":
        f = open(p, "wb")
        f.write(want)
        f.close()
    else:
        eq_bytes(rd(p), want, "content")


@persist
def fsynced_file(d, phase):
    """Explicitly fsynced, including the parent directory."""
    p = os.path.join(d, "f")
    want = rand_bytes("fsynced_file", 8192)
    if phase == "prepare":
        with open(p, "wb") as f:
            f.write(want)
            f.flush()
            os.fsync(f.fileno())
        dfd = os.open(d, os.O_RDONLY)
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)
    else:
        eq_bytes(rd(p), want, "content")


@persist
def multichunk_file(d, phase):
    """2 MiB across many 128 KiB chunks, checked by digest."""
    p = os.path.join(d, "big")
    want = rand_bytes("multichunk_file", 2 * 1024 * 1024)
    if phase == "prepare":
        wr(p, want)
    else:
        got = rd(p)
        eq(len(got), len(want), "size")
        eq(sha(got), sha(want), "sha256")


@persist
def chunk_boundary_write(d, phase):
    """A write straddling a chunk boundary must rejoin correctly on reload."""
    p = os.path.join(d, "f")
    want = rand_bytes("chunk_boundary_write", CHUNK * 2)
    if phase == "prepare":
        with open(p, "wb") as f:
            f.write(want[:CHUNK - 100])
            f.write(want[CHUNK - 100:CHUNK + 100])
            f.write(want[CHUNK + 100:])
    else:
        eq(sha(rd(p)), sha(want), "sha256")


@persist
def overwrite_middle(d, phase):
    """Overwrite inside an already-written chunk (read-modify-write)."""
    p = os.path.join(d, "f")
    base = bytearray(rand_bytes("overwrite_middle.base", CHUNK))
    patch = rand_bytes("overwrite_middle.patch", 777)
    base[5000:5000 + len(patch)] = patch
    if phase == "prepare":
        wr(p, rand_bytes("overwrite_middle.base", CHUNK))
        with open(p, "r+b") as f:
            f.seek(5000)
            f.write(patch)
    else:
        eq(sha(rd(p)), sha(bytes(base)), "sha256")


@persist
def appended_file(d, phase):
    """O_APPEND writes after the initial content."""
    p = os.path.join(d, "f")
    head = rand_bytes("appended_file.head", 3000)
    tail = rand_bytes("appended_file.tail", 5000)
    if phase == "prepare":
        wr(p, head)
        with open(p, "ab") as f:
            f.write(tail)
    else:
        eq_bytes(rd(p), head + tail, "content")


@persist
def sparse_file(d, phase):
    """A hole between two written extents must still read back as zeros."""
    p = os.path.join(d, "f")
    head = rand_bytes("sparse_file.head", 1000)
    tail = rand_bytes("sparse_file.tail", 1000)
    off = 4 * CHUNK
    if phase == "prepare":
        with open(p, "wb") as f:
            f.write(head)
            f.seek(off)
            f.write(tail)
    else:
        got = rd(p)
        eq(len(got), off + len(tail), "size")
        eq_bytes(got[:len(head)], head, "head")
        eq_bytes(got[off:], tail, "tail")
        if got[len(head):off] != b"\0" * (off - len(head)):
            raise Fail("hole did not read back as zeros")


@persist
def truncate_shrink(d, phase):
    """Truncated-away bytes must not come back."""
    p = os.path.join(d, "f")
    full = rand_bytes("truncate_shrink", 20000)
    if phase == "prepare":
        wr(p, full)
        os.truncate(p, 6000)
    else:
        got = rd(p)
        eq(len(got), 6000, "size")
        eq_bytes(got, full[:6000], "content")


@persist
def truncate_extend(d, phase):
    """Extending by truncate leaves a zero tail."""
    p = os.path.join(d, "f")
    head = rand_bytes("truncate_extend", 1234)
    size = 3 * CHUNK + 77
    if phase == "prepare":
        wr(p, head)
        os.truncate(p, size)
    else:
        got = rd(p)
        eq(len(got), size, "size")
        eq_bytes(got[:len(head)], head, "head")
        if got[len(head):] != b"\0" * (size - len(head)):
            raise Fail("extended tail is not zeros")


@persist
def empty_file(d, phase):
    """A zero-byte file is still a file."""
    p = os.path.join(d, "f")
    if phase == "prepare":
        open(p, "wb").close()
    else:
        eq(os.stat(p).st_size, 0, "size")
        if not statmod.S_ISREG(os.stat(p).st_mode):
            raise Fail("not a regular file after remount")


@persist
def dd_written_file(d, phase):
    """Written by a real dd(1) rather than by Python."""
    p = os.path.join(d, "f")
    src = os.path.join(d, "src")
    want = rand_bytes("dd_written_file", 512 * 1024)
    if phase == "prepare":
        wr(src, want)
        sh(["dd", "if=" + src, "of=" + p, "bs=64K", "conv=fsync"])
    else:
        eq(sha(rd(p)), sha(want), "sha256")


# --------------------------------------------------------------------------
# Namespace
# --------------------------------------------------------------------------

@persist
def many_files_in_one_dir(d, phase):
    """500 files must ALL come back, by listing and by content.

    This is the shape that caught a real ~5% silent create loss: the creates
    were acknowledged, so only a full recount reveals the gap. Checking the
    listing and the contents separately distinguishes a lost file from a file
    that exists but is missing from readdir.
    """
    n = 500
    sub = os.path.join(d, "many")
    names = ["f%04d" % i for i in range(n)]
    if phase == "prepare":
        os.makedirs(sub, exist_ok=True)
        for i, nm in enumerate(names):
            wr(os.path.join(sub, nm), rand_bytes("many:%d" % i, 64))
    else:
        listed = set(os.listdir(sub))
        missing = [nm for nm in names if nm not in listed]
        extra = sorted(listed - set(names))
        if missing or extra:
            raise Fail("listing: %d/%d present, missing=%s extra=%s" %
                       (len(listed), n, missing[:5], extra[:5]))
        for i, nm in enumerate(names):
            eq_bytes(rd(os.path.join(sub, nm)), rand_bytes("many:%d" % i, 64),
                     "content of " + nm)


@persist
def deep_tree(d, phase):
    """A deep path and its leaf file."""
    depth = 40
    rel = os.path.join(*["lvl%02d" % i for i in range(depth)])
    leaf = os.path.join(d, rel, "leaf")
    want = rand_bytes("deep_tree", 300)
    if phase == "prepare":
        os.makedirs(os.path.join(d, rel), exist_ok=True)
        wr(leaf, want)
    else:
        eq_bytes(rd(leaf), want, "content")


@persist
def strange_names(d, phase):
    """Unusual but legal filenames survive the round trip."""
    names = ["sp ace", "tab\tchar", "nl\nchar", "quote'\"", "back\\slash",
             "uni\u00e9\u4e2d\u6587", "dash-", "dot.dot", ".hidden",
             "\u00e9" * 60, "!@#$%^&()[]{}"]
    if phase == "prepare":
        for i, nm in enumerate(names):
            wr(os.path.join(d, nm), rand_bytes("strange:%d" % i, 128))
    else:
        listed = set(os.listdir(d))
        for i, nm in enumerate(names):
            if nm not in listed:
                raise Fail("name %r missing from readdir" % nm)
            eq_bytes(rd(os.path.join(d, nm)), rand_bytes("strange:%d" % i, 128),
                     "content of %r" % nm)


@persist
def renamed_file(d, phase):
    """After a rename the new name has the data and the old name is gone."""
    old = os.path.join(d, "old")
    new = os.path.join(d, "new")
    want = rand_bytes("renamed_file", 2048)
    if phase == "prepare":
        wr(old, want)
        os.rename(old, new)
    else:
        eq_bytes(rd(new), want, "content")
        if os.path.exists(old):
            raise Fail("old name reappeared after remount")


@persist
def renamed_across_dirs(d, phase):
    """A cross-directory rename survives, including the emptied source."""
    a, b = os.path.join(d, "a"), os.path.join(d, "b")
    want = rand_bytes("renamed_across_dirs", 4096)
    if phase == "prepare":
        os.makedirs(a, exist_ok=True)
        os.makedirs(b, exist_ok=True)
        wr(os.path.join(a, "x"), want)
        os.rename(os.path.join(a, "x"), os.path.join(b, "x"))
    else:
        eq_bytes(rd(os.path.join(b, "x")), want, "content")
        eq(os.listdir(a), [], "source dir should be empty")


@persist
def unlinked_file(d, phase):
    """A deleted file stays deleted, and its neighbour is untouched."""
    gone = os.path.join(d, "gone")
    keep = os.path.join(d, "keep")
    want = rand_bytes("unlinked_file", 1024)
    if phase == "prepare":
        wr(gone, rand_bytes("unlinked_file.gone", 1024))
        wr(keep, want)
        os.unlink(gone)
    else:
        if os.path.exists(gone):
            raise Fail("unlinked file came back after remount")
        eq_bytes(rd(keep), want, "surviving file")


@persist
def removed_dir(d, phase):
    """An rmdir'd directory stays gone."""
    sub = os.path.join(d, "sub")
    if phase == "prepare":
        os.makedirs(sub, exist_ok=True)
        os.rmdir(sub)
    else:
        if os.path.exists(sub):
            raise Fail("rmdir'd directory came back after remount")


@persist
def hardlinks(d, phase):
    """Both names, one inode, nlink=2, same content."""
    a, b = os.path.join(d, "a"), os.path.join(d, "b")
    want = rand_bytes("hardlinks", 4096)
    if phase == "prepare":
        wr(a, want)
        os.link(a, b)
    else:
        eq_bytes(rd(a), want, "content via a")
        eq_bytes(rd(b), want, "content via b")
        sa, sb = os.stat(a), os.stat(b)
        eq(sa.st_ino, sb.st_ino, "same inode")
        eq(sa.st_nlink, 2, "nlink")


@persist
def hardlink_after_unlink(d, phase):
    """Removing one link keeps the data reachable through the other."""
    a, b = os.path.join(d, "a"), os.path.join(d, "b")
    want = rand_bytes("hardlink_after_unlink", 2048)
    if phase == "prepare":
        wr(a, want)
        os.link(a, b)
        os.unlink(a)
    else:
        if os.path.exists(a):
            raise Fail("unlinked link came back")
        eq_bytes(rd(b), want, "content via surviving link")
        eq(os.stat(b).st_nlink, 1, "nlink")


@persist
def symlinks(d, phase):
    """Symlink target text is preserved verbatim, including a dangling one."""
    want = rand_bytes("symlinks", 256)
    tgt = os.path.join(d, "target")
    if phase == "prepare":
        wr(tgt, want)
        os.symlink("target", os.path.join(d, "rel"))
        os.symlink(tgt, os.path.join(d, "abs"))
        os.symlink("../nowhere/at/all", os.path.join(d, "dangling"))
    else:
        eq(os.readlink(os.path.join(d, "rel")), "target", "relative target")
        eq(os.readlink(os.path.join(d, "abs")), tgt, "absolute target")
        eq(os.readlink(os.path.join(d, "dangling")), "../nowhere/at/all",
           "dangling target")
        eq_bytes(rd(os.path.join(d, "rel")), want, "content through symlink")


# --------------------------------------------------------------------------
# Metadata
# --------------------------------------------------------------------------

@persist
def file_mode(d, phase):
    """Permission bits survive."""
    modes = [0o600, 0o644, 0o755, 0o400, 0o777, 0o000]
    if phase == "prepare":
        for m in modes:
            p = os.path.join(d, "m%04o" % m)
            wr(p, b"x")
            os.chmod(p, m)
    else:
        for m in modes:
            got = statmod.S_IMODE(os.stat(os.path.join(d, "m%04o" % m)).st_mode)
            eq(got, m, "mode of m%04o" % m)


@persist
def dir_mode(d, phase):
    """Directory permission bits survive."""
    if phase == "prepare":
        p = os.path.join(d, "dir")
        os.makedirs(p, exist_ok=True)
        os.chmod(p, 0o711)
    else:
        got = statmod.S_IMODE(os.stat(os.path.join(d, "dir")).st_mode)
        eq(got, 0o711, "dir mode")


# Fixed by the test, not copied from a pre-remount stat. A filesystem that
# stores the wrong size or truncates the nanoseconds must not get to define
# its own expected value.
_MTIME_NS = 1600000000 * 1000000000 + 123456789


@persist
def size_and_mtime(d, phase):
    """st_size and st_mtime_ns survive as the values this test set."""
    p = os.path.join(d, "f")
    want = rand_bytes("size_and_mtime", 12345)
    if phase == "prepare":
        wr(p, want)
        os.utime(p, ns=(_MTIME_NS, _MTIME_NS))
    else:
        st = os.stat(p)
        eq(st.st_size, 12345, "size")
        eq(st.st_mtime_ns, _MTIME_NS, "mtime_ns")


@persist
def large_file_size_only(d, phase):
    """A 64 MiB file reports the right size and correct bytes at both ends.

    Reading all of it back on every run would dominate the suite, so this
    checks the size plus the first and last chunk -- enough to catch a
    truncated or zero-filled reload.
    """
    p = os.path.join(d, "big")
    size = 64 * 1024 * 1024
    head = rand_bytes("large_file.head", CHUNK)
    tail = rand_bytes("large_file.tail", CHUNK)
    if phase == "prepare":
        with open(p, "wb") as f:
            f.write(head)
            f.seek(size - CHUNK)
            f.write(tail)
    else:
        eq(os.stat(p).st_size, size, "size")
        with open(p, "rb") as f:
            eq_bytes(f.read(CHUNK), head, "head chunk")
            f.seek(size - CHUNK)
            eq_bytes(f.read(CHUNK), tail, "tail chunk")


# --------------------------------------------------------------------------
# Runner
# --------------------------------------------------------------------------

def _fsync_dir(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _fsync_ancestry(start, stop):
    """fsync start and each parent through stop. Errors propagate."""
    path = os.path.abspath(start)
    stop = os.path.abspath(stop)
    seen = set()
    while path not in seen:
        seen.add(path)
        _fsync_dir(path)
        if path == stop:
            return
        parent = os.path.dirname(path)
        if parent == path:
            return
        path = parent


def _capture_paths(mnt, results):
    """Absolute paths from the caller's cwd, before this process chdirs."""
    mnt = os.path.abspath(mnt)
    if results:
        results = os.path.abspath(results)
    return mnt, results


def _maybe_hold(where):
    """Test hook. EFS_PERSIST_HOLD=start|after-result sleeps in this worker.

    A same-group child ignores SIGTERM, so cancelling the runner has to
    kill the process group and not only the leader.
    """
    if os.environ.get("EFS_PERSIST_HOLD") != where:
        return
    seconds = float(os.environ.get("EFS_PERSIST_HOLD_S", "30"))
    pidfile = os.environ.get("EFS_PERSIST_HOLD_PID")
    child = os.fork()
    if child == 0:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        time.sleep(seconds)
        os._exit(0)
    if pidfile:
        with open(pidfile, "w") as f:
            f.write("%s %s\n" % (os.getpid(), child))
            f.flush()
            os.fsync(f.fileno())
    time.sleep(seconds)


def _worker_main(name, tdir, phase, mnt, result):
    os.chdir("/tmp")
    _maybe_hold("start")
    fns = dict(TESTS)
    try:
        if name not in fns:
            _write_status(result, "FAIL", "unknown test %s" % name)
            os._exit(1)
        fns[name](tdir, phase)
        if phase == "prepare":
            _fsync_ancestry(tdir, mnt)
        _write_status(result, "PASS", "")
        _maybe_hold("after-result")
    except Fail as e:
        try:
            _write_status(result, "FAIL", str(e))
        except Exception:
            pass
    except Exception as e:  # noqa: BLE001
        try:
            _write_status(result, "FAIL", "%s: %s" % (type(e).__name__, e))
        except Exception:
            pass
    os._exit(0)


def _self_test():
    fails = []
    td = tempfile.mkdtemp(prefix="persist-self-")
    old = os.getcwd()
    try:
        os.chdir(td)
        os.mkdir("mnt")
        mnt, results = _capture_paths("mnt", "out.tsv")
        os.chdir("/")
        want_mnt = os.path.realpath(os.path.join(td, "mnt"))
        want_out = os.path.realpath(os.path.join(td, "out.tsv"))
        if os.path.realpath(mnt) != want_mnt or os.path.realpath(results) != want_out:
            fails.append("relative paths were not captured before chdir")
        if not os.path.isdir(mnt):
            fails.append("captured mount path is not a directory")
    finally:
        os.chdir(old)

    real_fsync = os.fsync
    real_open = os.open

    def check_raises(label, patch, target):
        patch()
        try:
            try:
                fsynced_file(td, "prepare")
            except OSError as e:
                if e.errno != errno.EIO:
                    fails.append("%s: errno %s" % (label, e.errno))
            except Fail as e:
                fails.append("%s became Fail (swallowed into the test?): %s" %
                             (label, e))
            else:
                fails.append("%s was swallowed and prepare returned" % label)
        finally:
            os.fsync = real_fsync
            os.open = real_open

    def patch_dir_fsync():
        def fsync(fd):
            if statmod.S_ISDIR(os.fstat(fd).st_mode):
                raise OSError(errno.EIO, "injected")
            return real_fsync(fd)
        os.fsync = fsync

    def patch_file_fsync():
        def fsync(fd):
            if not statmod.S_ISDIR(os.fstat(fd).st_mode):
                raise OSError(errno.EIO, "injected")
            return real_fsync(fd)
        os.fsync = fsync

    def patch_dir_open():
        def open_(path, *a, **k):
            if path == td:
                raise OSError(errno.EIO, "injected")
            return real_open(path, *a, **k)
        os.open = open_

    check_raises("dir fsync", patch_dir_fsync, td)
    check_raises("file fsync", patch_file_fsync, td)
    check_raises("dir open", patch_dir_open, td)

    def _pids_alive(path):
        if not os.path.exists(path):
            return []
        alive = []
        for tok in open(path).read().split():
            pid = int(tok)
            try:
                os.kill(pid, 0)
            except OSError as e:
                if e.errno != errno.ESRCH:
                    alive.append(pid)
            else:
                alive.append(pid)
        return alive

    def _row_status(path, name):
        if not os.path.exists(path):
            return None
        prefix = name + "\t"
        for line in open(path):
            if line.startswith(prefix):
                return line.split("\t", 2)[1]
        return None

    def _cli(mnt, phase, tag, when, results):
        pidfile = results + ".pids"
        env = os.environ.copy()
        env["EFS_PERSIST_HOLD_S"] = "30"
        env["EFS_PERSIST_HOLD_PID"] = pidfile
        env["EFS_PERSIST_HOLD"] = "after-result" if when == "after-result" else "start"
        if when == "raise":
            env["EFS_PERSIST_RAISE"] = "1"
        proc = subprocess.Popen(
            [sys.executable, os.path.abspath(__file__), mnt,
             "--phase", phase, "--filter", "small_file", "--tag", tag,
             "--results", results, "--timeout-s", "20", "--keep"],
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True)
        try:
            if when == "immediate":
                os.kill(proc.pid, signal.SIGTERM)
            elif when != "raise":
                deadline = time.monotonic() + 8
                while (time.monotonic() < deadline and
                       not os.path.exists(pidfile)):
                    if proc.poll() is not None:
                        break
                    time.sleep(0.02)
                if proc.poll() is None:
                    os.kill(proc.pid, signal.SIGTERM)
            try:
                rc = proc.wait(timeout=12)
            except subprocess.TimeoutExpired:
                _kill_pgid(proc.pid)
                rc = proc.wait(timeout=2)
            time.sleep(0.2)
            alive = _pids_alive(pidfile)
            return rc, alive, _row_status(results, "small_file")
        finally:
            if proc.poll() is None:
                _kill_pgid(proc.pid)
            for pid in _pids_alive(pidfile):
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass

    host = subprocess.run(["hostname", "-s"], stdout=subprocess.PIPE
                          ).stdout.decode().strip()
    mnt = tempfile.mkdtemp(prefix="persist-cli-")
    try:
        prep = subprocess.run(
            [sys.executable, os.path.abspath(__file__), mnt,
             "--phase", "prepare", "--filter", "small_file",
             "--tag", "cli", "--timeout-s", "30", "--keep"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if prep.returncode != 0:
            fails.append("prepare before cancel tests rc=%s" % prep.returncode)
        cases = (
            ("prepare", "immediate", "cli"),
            ("prepare", "start", "cli"),
            ("prepare", "after-result", "cli"),
            ("prepare", "raise", "cli"),
            ("verify", "start", "cliv"),
        )
        vprep = subprocess.run(
            [sys.executable, os.path.abspath(__file__), mnt,
             "--phase", "prepare", "--filter", "small_file",
             "--tag", "cliv", "--timeout-s", "30", "--keep"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if vprep.returncode != 0:
            fails.append("verify setup prepare rc=%s" % vprep.returncode)
        for phase, when, tag in cases:
            results = os.path.join(td, "cli-%s-%s.tsv" % (phase, when))
            rc, alive, status = _cli(mnt, phase, tag, when, results)
            if rc == 0:
                fails.append("%s/%s exited 0" % (phase, when))
            if alive:
                fails.append("%s/%s left workers %s" % (phase, when, alive))
            if when != "immediate" and status == "PASS":
                fails.append("%s/%s recorded PASS (%s)" % (phase, when, status))
            if when in ("start", "after-result", "raise") and status is None:
                fails.append("%s/%s wrote no row for small_file" % (phase, when))
    finally:
        shutil.rmtree(mnt, ignore_errors=True)
        base = os.path.join(mnt, "posix-persist-%s-cli" % host)
        shutil.rmtree(base, ignore_errors=True)

    if fails:
        for msg in fails:
            print("FAIL " + msg)
        return 1
    print("posix_persist self-test: pass")
    return 0


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    if args == ["--self-test"]:
        return _self_test()
    if args[:1] == ["--worker"]:
        if len(args) != 6:
            print("usage: --worker NAME TDIR PHASE MNT RESULT", file=sys.stderr)
            return 2
        _worker_main(args[1], args[2], args[3], args[4], args[5])
        return 1
    if not args:
        print(__doc__)
        return 2
    mnt = args[0]
    phase = None
    results_file = None
    tag = None
    keep = False
    filt = None
    test_timeout = int(os.environ.get("POSIX_TEST_SEC", "60"))
    i = 1
    while i < len(args):
        if args[i] == "--phase":
            phase = args[i + 1]
            i += 2
        elif args[i] == "--results":
            results_file = args[i + 1]
            i += 2
        elif args[i] == "--tag":
            tag = args[i + 1]
            i += 2
        elif args[i] == "--filter":
            filt = args[i + 1]
            i += 2
        elif args[i] == "--timeout-s":
            test_timeout = int(args[i + 1])
            i += 2
        elif args[i] == "--keep":
            keep = True
            i += 1
        else:
            i += 1

    mnt, results_file = _capture_paths(mnt, results_file)

    if phase not in ("prepare", "verify"):
        print("ERROR: --phase must be 'prepare' or 'verify'")
        return 2
    if not os.path.isdir(mnt):
        print("ERROR: %s is not a mounted directory" % mnt)
        return 2

    host = subprocess.run(["hostname", "-s"], stdout=subprocess.PIPE
                          ).stdout.decode().strip()
    # The base directory must be DERIVED, not random: verify runs in a
    # different process after a remount and has to find what prepare wrote.
    base = os.path.join(mnt, "posix-persist-%s%s" %
                        (host, ("-" + tag) if tag else ""))

    if phase == "prepare":
        # Only prepare clears the tree. If verify did any cleanup first it
        # could destroy the very evidence it exists to check.
        shutil.rmtree(base, ignore_errors=True)
        os.makedirs(base, exist_ok=True)
    elif not os.path.isdir(base):
        print("ERROR: %s missing — run --phase prepare first" % base)
        return 2

    # A cwd inside the mount blocks the unmount that has to happen next.
    os.chdir("/tmp")

    selected = [(n, fn) for n, fn in TESTS if not filt or filt in n]
    npass = nfail = 0
    t0 = time.time()
    by_name = {}
    mu = threading.Lock()

    def flush_tsv():
        if not results_file:
            return
        tmp = results_file + ".tmp"
        with open(tmp, "w") as f:
            f.write("# posix-persist host=%s mnt=%s phase=%s %s\n" %
                    (host, mnt, phase,
                     time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())))
            f.write("test\tresult\tdetail\n")
            for n, _fn in selected:
                if n in by_name:
                    f.write(format_result_line(n, by_name[n][0], by_name[n][1]))
            f.write("# summary pass=%d fail=%d skip=0 total=%d dur=%.1f\n" %
                    (npass, nfail, npass + nfail, time.time() - t0))
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, results_file)

    def record(name, status, detail):
        nonlocal npass, nfail
        with mu:
            by_name[name] = (status, detail)
            if status == "PASS":
                npass += 1
                print("pass %-32s" % name, flush=True)
            elif status == "NOTRUN":
                nfail += 1
                print("NOTRUN %-32s %s" % (name, detail), flush=True)
            else:
                nfail += 1
                print("FAIL %-32s %s" % (name, detail), flush=True)
            flush_tsv()

    script = os.path.abspath(__file__)
    runnable = []
    for name, _fn in selected:
        tdir = os.path.join(base, name)
        if phase == "prepare":
            os.makedirs(tdir, exist_ok=True)
        elif not os.path.isdir(tdir):
            record(name, "FAIL", "test directory vanished across the remount")
            continue
        runnable.append((name, test_timeout, tdir))

    live = []

    def _cut(signum, _frame):
        # An outer timeout must not leave a worker writing across the
        # unmount that separates prepare from verify. Kill every group
        # still registered, then leave a row for every test not finished.
        nonlocal nfail
        for pid in list(live):
            _kill_pgid(pid)
        try:
            for name, _fn in selected:
                if name not in by_name:
                    by_name[name] = ("NOTRUN",
                                     "runner cut by signal %d" % signum)
                    nfail += 1
            flush_tsv()
        except Exception:
            pass
        os._exit(128 + signum)

    signal.signal(signal.SIGTERM, _cut)
    signal.signal(signal.SIGINT, _cut)

    def spawn_test(name, tdir):
        fd, result = tempfile.mkstemp(prefix="persist-result-")
        os.close(fd)
        proc = subprocess.Popen(
            [sys.executable, script, "--worker", name, tdir, phase, mnt, result],
            start_new_session=True)
        # Register before anything else so a failure here is still cancelled.
        live.append(proc.pid)
        if os.environ.get("EFS_PERSIST_RAISE"):
            raise RuntimeError("injected runner failure")
        return proc, result

    # One test at a time. The next test starts only after this process group
    # has been reaped. A worker that survives SIGKILL ends the phase; the
    # tree is left in place for an external supervisor.
    stuck = None
    left = []
    try:
        stuck, left = _run_pool(
            runnable, 1, spawn_test,
            lambda name, status, detail: record(name, status, detail) or False,
            live=live)
    except Exception as exc:
        for pid in list(live):
            _kill_pgid(pid)
        for name, _fn in selected:
            if name not in by_name:
                try:
                    record(name, "NOTRUN", "runner exception: %s" % exc)
                except Exception:
                    by_name[name] = ("NOTRUN", "runner exception")
                    nfail += 1
        flush_tsv()
        os._exit(1)
    if stuck:
        for name, _timeout, _tdir in left:
            record(name, "NOTRUN", "incomplete: timed-out worker still alive")
    elif phase == "verify" and not keep:
        shutil.rmtree(base, ignore_errors=True)

    print("\n%s: pass=%d fail=%d total=%d in %.1fs" %
          (phase, npass, nfail, npass + nfail, time.time() - t0), flush=True)
    flush_tsv()
    # os._exit: a lingering FUSE fd in a daemon thread can hang interpreter
    # shutdown, and the TSV is already on disk.
    os._exit(0 if nfail == 0 else 1)


if __name__ == "__main__":
    sys.exit(main())
