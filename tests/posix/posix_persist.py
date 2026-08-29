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
import hashlib
import os
import shutil
import stat as statmod
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# Reuse the suite's helpers rather than copying them. rand_bytes in particular
# must be the SAME generator: a divergent copy would silently compare a file
# against bytes it was never written with.
from posix_suite import Fail, eq, rand_bytes, rd, sh, wr  # noqa: E402

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
        except OSError:
            pass
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


@persist
def size_and_mtime(d, phase):
    """st_size and st_mtime are the same values after the remount.

    The prepare phase records what it observed into a file that the verify
    phase reads back, so a drifting mtime is caught rather than a mtime that
    merely looks plausible.
    """
    p = os.path.join(d, "f")
    stamp = os.path.join(d, "stamp")
    want = rand_bytes("size_and_mtime", 12345)
    if phase == "prepare":
        wr(p, want)
        os.utime(p, (1600000000, 1600000000))
        st = os.stat(p)
        wr(stamp, ("%d %d" % (st.st_size, int(st.st_mtime))).encode())
    else:
        pre_size, pre_mtime = rd(stamp).decode().split()
        st = os.stat(p)
        eq(st.st_size, int(pre_size), "size")
        eq(int(st.st_mtime), int(pre_mtime), "mtime")


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

def main():
    args = sys.argv[1:]
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
                    f.write("%s\t%s\t%s\n" % (n, by_name[n][0],
                                              by_name[n][1].replace("\n", " ")))
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
            else:
                nfail += 1
                print("FAIL %-32s %s" % (name, detail), flush=True)
            flush_tsv()

    for name, fn in selected:
        tdir = os.path.join(base, name)
        if phase == "prepare":
            os.makedirs(tdir, exist_ok=True)
        elif not os.path.isdir(tdir):
            record(name, "FAIL", "test directory vanished across the remount")
            continue
        done = {}

        def run(fn=fn, tdir=tdir, done=done):
            try:
                fn(tdir, phase)
                done["r"] = ("PASS", "")
            except Fail as e:
                done["r"] = ("FAIL", str(e))
            except Exception as e:  # noqa: BLE001
                done["r"] = ("FAIL", "%s: %s" % (type(e).__name__, e))

        th = threading.Thread(target=run, daemon=True)
        th.start()
        th.join(test_timeout)
        if th.is_alive():
            record(name, "FAIL", "timeout after %ss" % test_timeout)
        else:
            st, detail = done.get("r", ("FAIL", "no result"))
            record(name, st, detail)

    if phase == "verify" and not keep:
        shutil.rmtree(base, ignore_errors=True)

    print("\n%s: pass=%d fail=%d total=%d in %.1fs" %
          (phase, npass, nfail, npass + nfail, time.time() - t0), flush=True)
    flush_tsv()
    # os._exit: a lingering FUSE fd in a daemon thread can hang interpreter
    # shutdown, and the TSV is already on disk.
    os._exit(0 if nfail == 0 else 1)


if __name__ == "__main__":
    sys.exit(main())
