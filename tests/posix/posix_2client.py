#!/usr/bin/env python3
"""Two-client POSIX visibility suite.

Single-mount posix_suite.py cannot see this: each efs-fuse keeps its own
metadata snapshot, and Phase 2b only dual-applies a mutation on the client
that issued it. A peer does not see mkdir/create/rename until remount
(Phase 2c). That is the IO-500 IOR-hard failure: all ranks O_CREAT one
shared file; the creator's peers get EIO/ENOENT instead of the inode.

Each test is a sequence of steps on side A then B (same export path, two
mounts / two hosts). B is never remounted during a test. A step of
("ab", (fn_a, fn_b)) runs both sides at the same time (layer-3 races).

Usage (on one host, two paths — XFS baseline uses the same path twice):
    posix_2client.py --local <mnt-a> <mnt-b> [--results FILE] [--filter SUB]

Usage (login node, two efs-fuse hosts; parent dirs must already be visible
on B — run --prepare on A, remount B, then --remote):
    posix_2client.py --prepare <mnt> [--parent NAME]
    posix_2client.py --remote <host-a> <host-b> --mnt <mnt> [--parent NAME] [--results FILE]

Internal (harness / --remote):
    posix_2client.py --exec <test> <a|b> <dir>
"""
from __future__ import print_function

import ctypes
import errno
import fcntl
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time


class _Flock(ctypes.Structure):
    """Linux struct flock (natural alignment: 4-byte pad after l_whence)."""
    _fields_ = [
        ("l_type", ctypes.c_int16),
        ("l_whence", ctypes.c_int16),
        ("l_start", ctypes.c_int64),
        ("l_len", ctypes.c_int64),
        ("l_pid", ctypes.c_int32),
    ]


def fcntl_range(fd, start, length, exclusive=True, nb=False, unlock=False):
    """F_SETLK with an absolute range. lockf() after lseek is not used:
    on this FUSE mount the kernel delivers l_start=0 for SEEK_CUR lockf
    even when f_pos is 4096 (probed: explicit F_SETLK is correct)."""
    fl = _Flock()
    if unlock:
        fl.l_type = fcntl.F_UNLCK
    elif exclusive:
        fl.l_type = fcntl.F_WRLCK
    else:
        fl.l_type = fcntl.F_RDLCK
    fl.l_whence = os.SEEK_SET
    fl.l_start = start
    fl.l_len = length
    op = fcntl.F_SETLK if (nb or unlock) else fcntl.F_SETLKW
    fcntl.fcntl(fd, op, fl)

PARENT = os.environ.get("EFS_POSIX2_PARENT") or "posix-2c"
TESTS = []          # list of (name, steps)  steps = [("a"|"b", fn), ...]
RESULTS = []


def test(fn):
    """Register a test. The function returns a list of (side, callback)."""
    steps = fn()
    TESTS.append((fn.__name__, steps, (fn.__doc__ or "").strip()))
    return fn


class Fail(Exception):
    def __init__(self, msg):
        super(Fail, self).__init__(msg)


# Errno a losing rename/unlink may see when the other side already took the
# name. Anything else (EIO, EACCES, EPERM) is a failed operation, not a race.
_RACE_LOST = (errno.ENOENT,)


def _race_call(label, fn):
    try:
        fn()
    except OSError as e:
        if e.errno not in _RACE_LOST:
            raise Fail("%s: errno %s (%s)" % (label, e.errno, e.strerror))
        return e.errno
    return 0


def _race_rc(d, side, rc):
    wr(os.path.join(d, ".race-%s" % side), str(rc).encode())


def _read_rc(d, side):
    p = os.path.join(d, ".race-%s" % side)
    if not os.path.exists(p):
        raise Fail("%s did not report a result" % side)
    return int(rd(p))


def wr(path, data):
    with open(path, "wb") as f:
        f.write(data)


def rd(path):
    with open(path, "rb") as f:
        return f.read()


def eq(got, want, what=""):
    if got != want:
        raise Fail("%s: got %r, want %r" % (what, got, want))


def pwrite_at(path, data, off, creat=False):
    flags = os.O_RDWR | (os.O_CREAT if creat else 0)
    fd = os.open(path, flags, 0o644)
    try:
        os.pwrite(fd, data, off)
        os.fsync(fd)
    finally:
        os.close(fd)


def pread_at(path, n, off):
    fd = os.open(path, os.O_RDONLY)
    try:
        return os.pread(fd, n, off)
    finally:
        os.close(fd)


def fsync_path(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _run_pair(fa, da, fb, db):
    """Run A and B callables at the same time (local two-mount / XFS)."""
    errors = []

    def wrap(fn, path, label):
        try:
            fn(path)
        except Exception as e:  # noqa: BLE001
            errors.append((label, e))

    ta = threading.Thread(target=wrap, args=(fa, da, "a"))
    tb = threading.Thread(target=wrap, args=(fb, db, "b"))
    ta.start()
    tb.start()
    ta.join()
    tb.join()
    if errors:
        label, e = errors[0]
        if isinstance(e, Fail):
            raise Fail("%s: %s" % (label, e))
        raise Fail("%s: %s: %s" % (label, type(e).__name__, e))


# ==========================================================================
# Tests — live visibility, no remount between A mutate and B check
# ==========================================================================

@test
def peer_mkdir_visible():
    """A mkdir; B must listdir + stat the new directory."""
    def a(d):
        os.mkdir(os.path.join(d, "sub"))

    def b(d):
        names = os.listdir(d)
        if "sub" not in names:
            raise Fail("B listdir missing 'sub' (saw %s)" % sorted(names))
        st = os.stat(os.path.join(d, "sub"))
        if not stat_isdir(st):
            raise Fail("B stat(sub) is not a directory")

    return [("a", a), ("b", b)]


@test
def peer_create_visible():
    """A creates an empty file; B must see it."""
    def a(d):
        wr(os.path.join(d, "f"), b"")

    def b(d):
        p = os.path.join(d, "f")
        if not os.path.exists(p):
            raise Fail("B does not see A's file")
        eq(os.path.getsize(p), 0, "empty size")

    return [("a", a), ("b", b)]


@test
def peer_write_read():
    """A writes bytes; B must read the same bytes (IOR-easy data)."""
    payload = b"hello-from-a\n" * 32

    def a(d):
        wr(os.path.join(d, "f"), payload)

    def b(d):
        p = os.path.join(d, "f")
        if not os.path.exists(p):
            raise Fail("B does not see A's file")
        eq(rd(p), payload, "B read")

    return [("a", a), ("b", b)]


@test
def peer_listdir_siblings():
    """A creates three files (IOR-easy file-per-proc); B readdir sees all."""
    def a(d):
        for i in range(3):
            wr(os.path.join(d, "ior_file_easy.%08d" % i), b"x" * 64)

    def b(d):
        names = set(os.listdir(d))
        want = set("ior_file_easy.%08d" % i for i in range(3))
        missing = want - names
        if missing:
            raise Fail("B listdir missing %s (saw %s)" %
                       (sorted(missing), sorted(names)))

    return [("a", a), ("b", b)]


@test
def peer_shared_creat():
    """IOR-hard: A O_CREAT the shared file; B O_CREAT|O_RDWR must not EIO."""
    def a(d):
        fd = os.open(os.path.join(d, "file"), os.O_CREAT | os.O_RDWR, 0o644)
        os.close(fd)

    def b(d):
        p = os.path.join(d, "file")
        try:
            fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
        except OSError as e:
            raise Fail("B open(O_CREAT|O_RDWR) of A's shared file: %s "
                       "(errno %s) — IOR-hard abort" % (e.strerror, e.errno))
        os.close(fd)

    return [("a", a), ("b", b)]


@test
def peer_open_existing():
    """A creates; B open() without O_CREAT (lookup, not create)."""
    def a(d):
        wr(os.path.join(d, "f"), b"exist")

    def b(d):
        p = os.path.join(d, "f")
        try:
            fd = os.open(p, os.O_RDWR)
        except OSError as e:
            raise Fail("B open(O_RDWR) of A's file: %s (errno %s)" %
                       (e.strerror, e.errno))
        os.close(fd)

    return [("a", a), ("b", b)]


@test
def peer_creat_excl_eexist():
    """A creates; B O_EXCL must get EEXIST, not a second inode or EIO."""
    def a(d):
        wr(os.path.join(d, "f"), b"first")

    def b(d):
        p = os.path.join(d, "f")
        try:
            fd = os.open(p, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o644)
        except OSError as e:
            if e.errno == errno.EEXIST:
                return
            raise Fail("B O_EXCL got errno %s (%s), want EEXIST" %
                       (e.errno, e.strerror))
        os.close(fd)
        raise Fail("B O_EXCL succeeded — peer create was invisible")

    return [("a", a), ("b", b)]


@test
def peer_stat_size():
    """A writes 1 MiB; B stat size must match."""
    n = 1 << 20

    def a(d):
        wr(os.path.join(d, "f"), b"Z" * n)

    def b(d):
        p = os.path.join(d, "f")
        try:
            sz = os.path.getsize(p)
        except OSError as e:
            raise Fail("B stat: %s" % e)
        if sz != n:
            raise Fail("B st_size=%d, want %d" % (sz, n))

    return [("a", a), ("b", b)]


@test
def peer_rename_visible():
    """A create + rename; B sees the new name, not the old one."""
    def a(d):
        wr(os.path.join(d, "old"), b"renamed")
        os.rename(os.path.join(d, "old"), os.path.join(d, "new"))

    def b(d):
        names = set(os.listdir(d))
        if "new" not in names:
            raise Fail("B listdir missing 'new' (saw %s)" % sorted(names))
        if "old" in names:
            raise Fail("B still sees 'old' after A's rename")
        eq(rd(os.path.join(d, "new")), b"renamed", "B read renamed")

    return [("a", a), ("b", b)]


@test
def peer_mkdir_then_create():
    """A mkdir + create inside; B reads the nested file."""
    def a(d):
        os.mkdir(os.path.join(d, "dir"))
        wr(os.path.join(d, "dir", "f"), b"nested")

    def b(d):
        p = os.path.join(d, "dir", "f")
        try:
            data = rd(p)
        except OSError as e:
            raise Fail("B read nested file: %s" % e)
        eq(data, b"nested", "nested")

    return [("a", a), ("b", b)]


@test
def peer_shared_pwrite():
    """IOR-hard: concurrent disjoint 4K pwrite+fsync; both ranges survive (I12)."""
    n = int(os.environ.get("EFS_N1_N", "500"))
    blk = 4096
    stride = 8192
    size = 64 * 1024 * 1024

    def a0(d):
        p = os.path.join(d, "n1")
        fd = os.open(p, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o644)
        try:
            os.ftruncate(fd, size)
            os.fsync(fd)
        finally:
            os.close(fd)

    def a(d):
        fd = os.open(os.path.join(d, "n1"), os.O_RDWR)
        buf = b"\xaa" * blk
        try:
            for i in range(n):
                os.pwrite(fd, buf, i * stride)
                os.fsync(fd)
        finally:
            os.close(fd)

    def b(d):
        p = os.path.join(d, "n1")
        try:
            fd = os.open(p, os.O_RDWR)
        except OSError as e:
            raise Fail("B open shared file: %s (errno %s)" %
                       (e.strerror, e.errno))
        buf = b"\xbb" * blk
        try:
            for i in range(n):
                os.pwrite(fd, buf, i * stride + blk)
                os.fsync(fd)
        finally:
            os.close(fd)

    def a2(d):
        fd = os.open(os.path.join(d, "n1"), os.O_RDONLY)
        lost = 0
        try:
            for i in range(n):
                aa = os.pread(fd, blk, i * stride)
                bb = os.pread(fd, blk, i * stride + blk)
                if aa != b"\xaa" * blk:
                    lost += 1
                if bb != b"\xbb" * blk:
                    lost += 1
        finally:
            os.close(fd)
        if lost:
            raise Fail("I12 N-1 lost %d of %d half-blocks" % (lost, n * 2))

    return [("a", a0), ("ab", (a, b)), ("a", a2)]


@test
def peer_unlink_gone():
    """A creates then unlinks; B must not see a leftover name.

    Weak if creates are already invisible — still catches a ghost name
    appearing only on B.
    """
    def a(d):
        wr(os.path.join(d, "gone"), b"x")
        os.unlink(os.path.join(d, "gone"))

    def b(d):
        if os.path.exists(os.path.join(d, "gone")):
            raise Fail("B still sees unlinked 'gone'")

    return [("a", a), ("b", b)]


@test
def peer_chmod_visible():
    """A chmod; B must see the new mode."""
    def a(d):
        p = os.path.join(d, "f")
        wr(p, b"x")
        os.chmod(p, 0o600)

    def b(d):
        mode = os.stat(os.path.join(d, "f")).st_mode & 0o777
        eq(mode, 0o600, "B mode")

    return [("a", a), ("b", b)]


@test
def peer_utimens_visible():
    """A utime; B must see the new mtime."""
    def a(d):
        p = os.path.join(d, "f")
        wr(p, b"x")
        os.utime(p, (1500000000, 1500000000))

    def b(d):
        eq(int(os.stat(os.path.join(d, "f")).st_mtime), 1500000000, "B mtime")

    return [("a", a), ("b", b)]


@test
def peer_dir_mtime_bump_visible():
    """A creates an entry in the shared dir; B must see the directory's
    mtime AND ctime advance (§7.4 — for a spread dir the bump lives in the
    dentry shard's dir lane and is reduced at stat)."""
    before = {}

    def b_before(d):
        st = os.stat(d)
        before["m"] = st.st_mtime_ns
        before["c"] = st.st_ctime_ns
        time.sleep(0.06)   # outside the 50 ms lookup-memo window (0j)

    def a(d):
        wr(os.path.join(d, "f"), b"x")

    def b_after(d):
        st = os.stat(d)
        if st.st_mtime_ns <= before["m"]:
            raise Fail("B: dir mtime did not advance after A's create "
                       "(%d -> %d)" % (before["m"], st.st_mtime_ns))
        if st.st_ctime_ns <= before["c"]:
            raise Fail("B: dir ctime did not advance after A's create "
                       "(%d -> %d)" % (before["c"], st.st_ctime_ns))

    return [("b", b_before), ("a", a), ("b", b_after)]


@test
def peer_truncate_visible():
    """A truncate; B must see the new size and prefix."""
    def a(d):
        p = os.path.join(d, "f")
        wr(p, b"0123456789")
        os.truncate(p, 4)

    def b(d):
        p = os.path.join(d, "f")
        eq(os.path.getsize(p), 4, "B size after truncate")
        eq(rd(p), b"0123", "B data after truncate")

    return [("a", a), ("b", b)]


@test
def peer_hardlink_visible():
    """A hardlink; B sees both names, same ino, shared data."""
    def a(d):
        wr(os.path.join(d, "f"), b"shared")
        os.link(os.path.join(d, "f"), os.path.join(d, "g"))

    def b(d):
        f = os.path.join(d, "f")
        g = os.path.join(d, "g")
        if not os.path.exists(g):
            raise Fail("B does not see hardlink g")
        eq(os.stat(f).st_ino, os.stat(g).st_ino, "B same ino")
        eq(os.stat(f).st_nlink, 2, "B nlink")
        eq(rd(g), b"shared", "B read via g")

    return [("a", a), ("b", b)]


@test
def peer_symlink_visible():
    """A symlink; B readlink + follow."""
    def a(d):
        wr(os.path.join(d, "t"), b"tgt")
        os.symlink("t", os.path.join(d, "l"))

    def b(d):
        l = os.path.join(d, "l")
        if not os.path.islink(l):
            raise Fail("B does not see symlink l")
        eq(os.readlink(l), "t", "B readlink")
        eq(rd(l), b"tgt", "B follow")

    return [("a", a), ("b", b)]


@test
def peer_negative_dentry():
    """B ENOENT, A create, B must see the file (no sticky negative dentry)."""
    def b1(d):
        try:
            os.stat(os.path.join(d, "later"))
        except OSError as e:
            if e.errno != errno.ENOENT:
                raise Fail("B stat later errno %s" % e.errno)
        else:
            raise Fail("later existed before A created it")

    def a(d):
        wr(os.path.join(d, "later"), b"now")

    def b2(d):
        eq(rd(os.path.join(d, "later")), b"now", "B after create")

    return [("b", b1), ("a", a), ("b", b2)]


@test
def peer_fsync_then_read():
    """A write+fsync; B reads the bytes."""
    payload = b"fsync-vis" * 64

    def a(d):
        fd = os.open(os.path.join(d, "f"), os.O_CREAT | os.O_WRONLY, 0o644)
        os.write(fd, payload)
        os.fsync(fd)
        os.close(fd)

    def b(d):
        eq(rd(os.path.join(d, "f")), payload, "B after A fsync")

    return [("a", a), ("b", b)]


@test
def peer_mkdir_rmdir_recreate():
    """A mkdir, rmdir, mkdir again; B sees the new directory."""
    def a(d):
        p = os.path.join(d, "sub")
        os.mkdir(p)
        os.rmdir(p)
        os.mkdir(p)
        wr(os.path.join(p, "inner"), b"y")

    def b(d):
        p = os.path.join(d, "sub")
        if not os.path.isdir(p):
            raise Fail("B does not see recreated sub")
        eq(rd(os.path.join(p, "inner")), b"y", "B inner")

    return [("a", a), ("b", b)]


@test
def peer_oappend():
    """A writes, B O_APPEND, A reads both lines (cross-client append)."""
    def a(d):
        wr(os.path.join(d, "f"), b"line1\n")

    def b(d):
        fd = os.open(os.path.join(d, "f"), os.O_WRONLY | os.O_APPEND)
        os.write(fd, b"line2\n")
        os.close(fd)

    def a2(d):
        eq(rd(os.path.join(d, "f")), b"line1\nline2\n", "A sees B O_APPEND")

    return [("a", a), ("b", b), ("a", a2)]


@test
def peer_rename_over():
    """A rename-over; B sees old name gone and dest content replaced."""
    def a(d):
        wr(os.path.join(d, "src"), b"NEW")
        wr(os.path.join(d, "dst"), b"OLD")
        os.rename(os.path.join(d, "src"), os.path.join(d, "dst"))

    def b(d):
        if os.path.exists(os.path.join(d, "src")):
            raise Fail("B still sees renamed-away src")
        eq(rd(os.path.join(d, "dst")), b"NEW", "B dest after rename-over")

    return [("a", a), ("b", b)]


@test
def peer_flock_exclusive():
    """A holds flock EX; B LOCK_EX|NB must fail; unlock lets B in."""
    def a(d):
        wr(os.path.join(d, "f"), b"x")
        _spawn_holder(d, "flock")

    def b(d):
        fd = os.open(os.path.join(d, "f"), os.O_RDWR)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as e:
            if e.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                raise Fail("B flock errno %s (want EAGAIN)" % e.errno)
        else:
            fcntl.flock(fd, fcntl.LOCK_UN)
            raise Fail("B acquired LOCK_EX while A held it")
        finally:
            os.close(fd)

    def a2(d):
        _holder_go(d)
        fd = os.open(os.path.join(d, "f"), os.O_RDWR)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.flock(fd, fcntl.LOCK_UN)
        except OSError as e:
            raise Fail("A could not LOCK_EX after holder released: %s" % e)
        finally:
            os.close(fd)

    return [("a", a), ("b", b), ("a", a2)]


@test
def peer_unlink_while_b_has_fd():
    """B holds an fd; A unlinks the name; B still reads the bytes."""
    def a(d):
        wr(os.path.join(d, "f"), b"still-here")

    def b(d):
        _spawn_holder(d, "holdfd")

    def a2(d):
        os.unlink(os.path.join(d, "f"))
        if os.path.exists(os.path.join(d, "f")):
            raise Fail("name still exists after unlink")

    def b2(d):
        data, nlink = _holder_go(d)
        eq(data, b"still-here", "B fd read after A unlink")
        eq(nlink, 0, "nlink of the unlinked open file")

    return [("a", a), ("b", b), ("a", a2), ("b", b2)]


@test
def peer_rmdir_gone():
    """A mkdir then rmdir; B must not see the directory."""
    def a(d):
        os.mkdir(os.path.join(d, "gone"))
        os.rmdir(os.path.join(d, "gone"))

    def b(d):
        if os.path.exists(os.path.join(d, "gone")):
            raise Fail("B still sees rmdir'd gone")
        names = os.listdir(d)
        if "gone" in names:
            raise Fail("B listdir still has gone")

    return [("a", a), ("b", b)]


@test
def peer_nlink_after_link():
    """A hardlink; B sees nlink=2 and the same ino."""
    def a(d):
        wr(os.path.join(d, "f"), b"x")
        os.link(os.path.join(d, "f"), os.path.join(d, "g"))

    def b(d):
        sf = os.stat(os.path.join(d, "f"))
        sg = os.stat(os.path.join(d, "g"))
        eq(sf.st_nlink, 2, "B nlink")
        eq(sf.st_ino, sg.st_ino, "B same ino")
        eq(rd(os.path.join(d, "g")), b"x", "B read via g")

    return [("a", a), ("b", b)]


@test
def peer_hardlink_write():
    """A writes via one name; B reads the other."""
    def a(d):
        wr(os.path.join(d, "f"), b"aaa")
        os.link(os.path.join(d, "f"), os.path.join(d, "g"))
        with open(os.path.join(d, "g"), "ab") as f:
            f.write(b"bbb")

    def b(d):
        eq(rd(os.path.join(d, "f")), b"aaabbb", "B via f")
        eq(rd(os.path.join(d, "g")), b"aaabbb", "B via g")

    return [("a", a), ("b", b)]


@test
def peer_rename_dir():
    """A rename a directory with a child; B sees the new path."""
    def a(d):
        os.mkdir(os.path.join(d, "old"))
        wr(os.path.join(d, "old", "f"), b"keep")
        os.rename(os.path.join(d, "old"), os.path.join(d, "new"))

    def b(d):
        if os.path.exists(os.path.join(d, "old")):
            raise Fail("B still sees old/")
        eq(rd(os.path.join(d, "new", "f")), b"keep", "B child after dir rename")

    return [("a", a), ("b", b)]


@test
def peer_o_trunc_visible():
    """A O_TRUNC + write; B sees the new size and bytes."""
    def a(d):
        wr(os.path.join(d, "f"), b"0123456789")
        fd = os.open(os.path.join(d, "f"), os.O_WRONLY | os.O_TRUNC)
        os.write(fd, b"Z")
        os.fsync(fd)
        os.close(fd)

    def b(d):
        eq(os.path.getsize(os.path.join(d, "f")), 1, "B size after O_TRUNC")
        eq(rd(os.path.join(d, "f")), b"Z", "B content after O_TRUNC")

    return [("a", a), ("b", b)]


@test
def peer_unlink_recreate():
    """A unlink + create same name; B sees the new inode/content."""
    def a(d):
        wr(os.path.join(d, "f"), b"old")
        os.unlink(os.path.join(d, "f"))
        wr(os.path.join(d, "f"), b"new")

    def b(d):
        eq(rd(os.path.join(d, "f")), b"new", "B sees recreated file")
        eq(os.path.getsize(os.path.join(d, "f")), 3, "B size")

    return [("a", a), ("b", b)]


@test
def peer_lstat_symlink_size():
    """A symlink; B lstat size is the target string length."""
    def a(d):
        wr(os.path.join(d, "t"), b"tgt")
        os.symlink("t", os.path.join(d, "l"))

    def b(d):
        l = os.path.join(d, "l")
        if not os.path.islink(l):
            raise Fail("B does not see symlink")
        eq(os.lstat(l).st_size, 1, "B symlink st_size")
        eq(os.readlink(l), "t", "B readlink")
        eq(rd(l), b"tgt", "B follow")

    return [("a", a), ("b", b)]


@test
def peer_listdir_after_unlink():
    """A creates two files and unlinks one; B readdir matches."""
    def a(d):
        wr(os.path.join(d, "keep"), b"k")
        wr(os.path.join(d, "drop"), b"d")
        os.unlink(os.path.join(d, "drop"))

    def b(d):
        names = set(os.listdir(d))
        if "keep" not in names:
            raise Fail("B missing keep (saw %s)" % sorted(names))
        if "drop" in names:
            raise Fail("B still lists drop")

    return [("a", a), ("b", b)]


@test
def peer_chmod_via_hardlink():
    """A chmod via one name; B sees the mode on the other."""
    def a(d):
        wr(os.path.join(d, "f"), b"x")
        os.link(os.path.join(d, "f"), os.path.join(d, "g"))
        os.chmod(os.path.join(d, "g"), 0o600)

    def b(d):
        eq(os.stat(os.path.join(d, "f")).st_mode & 0o777, 0o600, "B mode via f")
        eq(os.stat(os.path.join(d, "g")).st_mode & 0o777, 0o600, "B mode via g")

    return [("a", a), ("b", b)]


@test
def peer_sparse_size():
    """A pwrite at 1 MiB; B stat size and reads the hole."""
    def a(d):
        fd = os.open(os.path.join(d, "f"), os.O_CREAT | os.O_RDWR, 0o644)
        os.pwrite(fd, b"Z", 1 << 20)
        os.fsync(fd)
        os.close(fd)

    def b(d):
        p = os.path.join(d, "f")
        eq(os.path.getsize(p), (1 << 20) + 1, "B sparse size")
        fd = os.open(p, os.O_RDONLY)
        try:
            eq(os.pread(fd, 4, 0), b"\x00" * 4, "B hole zeros")
            eq(os.pread(fd, 1, 1 << 20), b"Z", "B data at 1MiB")
        finally:
            os.close(fd)

    return [("a", a), ("b", b)]


# ==========================================================================
# Adversarial: A and B mutate at the same time (layer 3)
# Step ("ab", (fn_a, fn_b)) runs both sides concurrently.
# ==========================================================================

@test
def peer_overlap_pwrite_same_range():
    """A and B pwrite the same 4 KiB. Final region is all-A or all-B, never torn."""
    def a0(d):
        pwrite_at(os.path.join(d, "f"), b"\x00" * 4096, 0, creat=True)

    def a(d):
        pwrite_at(os.path.join(d, "f"), b"A" * 4096, 0)

    def b(d):
        pwrite_at(os.path.join(d, "f"), b"B" * 4096, 0)

    def a2(d):
        data = pread_at(os.path.join(d, "f"), 4096, 0)
        if data != b"A" * 4096 and data != b"B" * 4096:
            raise Fail("torn same-range pwrite (mix of A/B)")

    return [("a", a0), ("ab", (a, b)), ("a", a2)]


@test
def peer_overlap_pwrite_partial():
    """A writes [0,8192); B writes [4096,12288). Ends exclusive; overlap serializes."""
    def a0(d):
        pwrite_at(os.path.join(d, "f"), b"\x00" * 12288, 0, creat=True)

    def a(d):
        pwrite_at(os.path.join(d, "f"), b"A" * 8192, 0)

    def b(d):
        pwrite_at(os.path.join(d, "f"), b"B" * 8192, 4096)

    def a2(d):
        data = pread_at(os.path.join(d, "f"), 12288, 0)
        if data[0:4096] != b"A" * 4096:
            raise Fail("[0,4096) is not A's exclusive range")
        if data[8192:12288] != b"B" * 4096:
            raise Fail("[8192,12288) is not B's exclusive range")
        mid = data[4096:8192]
        if mid != b"A" * 4096 and mid != b"B" * 4096:
            raise Fail("torn overlap [4096,8192)")

    return [("a", a0), ("ab", (a, b)), ("a", a2)]


@test
def peer_overlap_pwrite_chunk_straddle():
    """Two clients write across the 128 KiB chunk: 127 KiB and 129 KiB."""
    cs = 128 * 1024

    def a0(d):
        pwrite_at(os.path.join(d, "f"), b"\x00" * (cs + 4096), 0, creat=True)

    def a(d):
        pwrite_at(os.path.join(d, "f"), b"A" * 4096, cs - 1024)

    def b(d):
        pwrite_at(os.path.join(d, "f"), b"B" * 4096, cs - 1024 + 2048)

    def a2(d):
        # A: [cs-1024, cs+3072). B: [cs+1024, cs+5120).
        # Exclusive: [cs-1024, cs+1024) = A, [cs+3072, cs+5120) = B.
        lo = pread_at(os.path.join(d, "f"), 2048, cs - 1024)
        hi = pread_at(os.path.join(d, "f"), 2048, cs + 3072)
        if lo != b"A" * 2048:
            raise Fail("below-chunk exclusive range not A")
        if hi != b"B" * 2048:
            raise Fail("above-chunk exclusive range not B")
        mid = pread_at(os.path.join(d, "f"), 2048, cs + 1024)
        if mid != b"A" * 2048 and mid != b"B" * 2048:
            raise Fail("torn chunk-straddle overlap")

    return [("a", a0), ("ab", (a, b)), ("a", a2)]


@test
def peer_concurrent_append():
    """A and B each O_APPEND 400 unique records. All 800 land, none torn."""
    n = 400

    def a0(d):
        wr(os.path.join(d, "f"), b"")
        fsync_path(os.path.join(d, "f"))

    def a(d):
        fd = os.open(os.path.join(d, "f"), os.O_WRONLY | os.O_APPEND)
        try:
            for i in range(n):
                os.write(fd, ("A%04d\n" % i).encode())
        finally:
            os.close(fd)

    def b(d):
        fd = os.open(os.path.join(d, "f"), os.O_WRONLY | os.O_APPEND)
        try:
            for i in range(n):
                os.write(fd, ("B%04d\n" % i).encode())
        finally:
            os.close(fd)

    def a2(d):
        lines = rd(os.path.join(d, "f")).splitlines()
        eq(len(lines), 2 * n, "append record count")
        seen = {}
        for ln in lines:
            if len(ln) != 5 or ln[0] not in b"AB" or not ln[1:].isdigit():
                raise Fail("partial/corrupt record %r" % ln)
            if ln in seen:
                raise Fail("duplicate record %r" % ln)
            seen[ln] = 1
        for i in range(n):
            if ("A%04d" % i).encode() not in seen:
                raise Fail("missing A%04d" % i)
            if ("B%04d" % i).encode() not in seen:
                raise Fail("missing B%04d" % i)

    return [("a", a0), ("ab", (a, b)), ("a", a2)]


@test
def peer_append_while_truncate():
    """A O_APPENDs while B truncates. File stays readable; records are whole."""
    def a0(d):
        wr(os.path.join(d, "f"), b"")
        fsync_path(os.path.join(d, "f"))

    def a(d):
        fd = os.open(os.path.join(d, "f"), os.O_WRONLY | os.O_APPEND)
        try:
            for i in range(80):
                os.write(fd, ("A%04d\n" % i).encode())
        finally:
            os.close(fd)

    def b(d):
        p = os.path.join(d, "f")
        for _ in range(16):
            os.truncate(p, 0)
            time.sleep(0.01)

    def a2(d):
        data = rd(os.path.join(d, "f"))
        if data and not data.endswith(b"\n"):
            raise Fail("truncated mid-record: %r" % data[-16:])
        for ln in data.splitlines():
            if ln and (len(ln) != 5 or ln[0] != ord("A") or not ln[1:].isdigit()):
                raise Fail("corrupt leftover record %r" % ln)

    return [("a", a0), ("ab", (a, b)), ("a", a2)]


@test
def peer_disjoint_extend():
    """A pwrite at 1 MiB, B at 8 MiB. st_size is the max committed end."""
    def a0(d):
        wr(os.path.join(d, "f"), b"")

    def a(d):
        pwrite_at(os.path.join(d, "f"), b"A", 1 << 20)

    def b(d):
        pwrite_at(os.path.join(d, "f"), b"B", 8 << 20)

    def a2(d):
        p = os.path.join(d, "f")
        eq(os.path.getsize(p), (8 << 20) + 1, "size is max write end")
        eq(pread_at(p, 1, 1 << 20), b"A", "low extend survived")
        eq(pread_at(p, 1, 8 << 20), b"B", "high extend survived")

    return [("a", a0), ("ab", (a, b)), ("a", a2)]


@test
def peer_disjoint_extend_reverse_completion():
    """High offset lands first; later low-offset REPORT must not shrink size."""
    def a(d):
        pwrite_at(os.path.join(d, "f"), b"H", 8 << 20, creat=True)
        fsync_path(os.path.join(d, "f"))

    def b(d):
        pwrite_at(os.path.join(d, "f"), b"L", 1 << 20)

    def a2(d):
        p = os.path.join(d, "f")
        sz = os.path.getsize(p)
        if sz < (8 << 20) + 1:
            raise Fail("low-offset write shrank size to %d" % sz)
        eq(pread_at(p, 1, 8 << 20), b"H", "high byte")
        eq(pread_at(p, 1, 1 << 20), b"L", "low byte")

    return [("a", a), ("b", b), ("a", a2)]


@test
def peer_extend_and_truncate():
    """A extends to 1 MiB while B truncate(4k). Size is one of the two ends."""
    def a0(d):
        wr(os.path.join(d, "f"), b"x" * 8192)
        fsync_path(os.path.join(d, "f"))

    def a(d):
        pwrite_at(os.path.join(d, "f"), b"A", 1 << 20)

    def b(d):
        os.truncate(os.path.join(d, "f"), 4096)

    def a2(d):
        p = os.path.join(d, "f")
        sz = os.path.getsize(p)
        if sz not in (4096, (1 << 20) + 1):
            raise Fail("illegal size %d (want 4096 or 1MiB+1)" % sz)
        if sz == 4096:
            eq(len(rd(p)), 4096, "trunc data")
        else:
            eq(pread_at(p, 1, 1 << 20), b"A", "extend survived truncate")

    return [("a", a0), ("ab", (a, b)), ("a", a2)]


@test
def peer_stat_during_extend():
    """B stats while A extends. Size is 0 or the committed end, never a hole."""
    def a0(d):
        wr(os.path.join(d, "f"), b"")

    def a(d):
        pwrite_at(os.path.join(d, "f"), b"Z", 2 << 20)

    def b(d):
        p = os.path.join(d, "f")
        sizes = []
        for _ in range(40):
            try:
                sizes.append(os.path.getsize(p))
            except OSError as e:
                raise Fail("B stat during extend: %s" % e)
            time.sleep(0.01)
        for sz in sizes:
            if sz not in (0, (2 << 20) + 1):
                raise Fail("B saw illegal size %d (want 0 or 2MiB+1)" % sz)

    return [("a", a0), ("ab", (a, b))]


@test
def peer_trunc_then_pwrite_old_fd():
    """A holds fd; B truncate(4k); A pwrite at 1 MiB on the old fd."""
    def a0(d):
        pwrite_at(os.path.join(d, "f"), b"X" * 8192, 0, creat=True)

    def a(d):
        _spawn_holder(d, "pwrite1m")

    def b(d):
        os.truncate(os.path.join(d, "f"), 4096)

    def a2(d):
        _holder_go(d)
        sz = os.path.getsize(os.path.join(d, "f"))
        if sz != (1 << 20) + 1:
            raise Fail("size after old-fd pwrite past trunc: %d" % sz)

    return [("a", a0), ("a", a), ("b", b), ("a", a2)]


@test
def peer_trunc_zero_then_high_pwrite():
    """A truncate 0; B pwrite at 1 MiB. No stale prefix below the hole."""
    def a0(d):
        pwrite_at(os.path.join(d, "f"), b"OLD" * 100, 0, creat=True)
        fsync_path(os.path.join(d, "f"))

    def a(d):
        os.truncate(os.path.join(d, "f"), 0)

    def b(d):
        pwrite_at(os.path.join(d, "f"), b"N", 1 << 20)

    def a2(d):
        p = os.path.join(d, "f")
        eq(os.path.getsize(p), (1 << 20) + 1, "size after trunc0+high")
        eq(pread_at(p, 3, 0), b"\x00\x00\x00", "no stale prefix")
        eq(pread_at(p, 1, 1 << 20), b"N", "high byte")

    return [("a", a0), ("a", a), ("b", b), ("a", a2)]


@test
def peer_pwrite_then_peer_truncate():
    """A pwrite 1 MiB; B truncate 4 KiB. B's size and prefix win if later."""
    def a(d):
        pwrite_at(os.path.join(d, "f"), b"A" * 4096, 1 << 20, creat=True)

    def b(d):
        os.truncate(os.path.join(d, "f"), 4096)

    def a2(d):
        p = os.path.join(d, "f")
        eq(os.path.getsize(p), 4096, "size after later truncate")
        data = rd(p)
        eq(len(data), 4096, "trunc length")
        if b"A" in data:
            raise Fail("extent past EOF survived truncate")

    return [("a", a), ("b", b), ("a", a2)]


@test
def peer_rename_same_src_two_dst():
    """A rename x→y and B rename x→z. Exactly one dest consumes x."""
    def a0(d):
        wr(os.path.join(d, "x"), b"src")
        fsync_path(os.path.join(d, "x"))

    def a(d):
        rc = _race_call("rename x→y", lambda: os.rename(
            os.path.join(d, "x"), os.path.join(d, "y")))
        _race_rc(d, "a", rc)

    def b(d):
        rc = _race_call("rename x→z", lambda: os.rename(
            os.path.join(d, "x"), os.path.join(d, "z")))
        _race_rc(d, "b", rc)

    def a2(d):
        arc = _read_rc(d, "a")
        brc = _read_rc(d, "b")
        y = os.path.exists(os.path.join(d, "y"))
        z = os.path.exists(os.path.join(d, "z"))
        x = os.path.exists(os.path.join(d, "x"))
        if arc == 0 and brc == 0:
            raise Fail("both renames reported success")
        if arc != 0 and brc != 0:
            raise Fail("neither rename succeeded (a=%s b=%s)" % (arc, brc))
        if (arc == 0) != y or (brc == 0) != z:
            raise Fail("result bits a=%s b=%s do not match y=%s z=%s" %
                       (arc, brc, y, z))
        if y and z:
            raise Fail("both y and z exist (src consumed twice)")
        if not y and not z:
            if x:
                raise Fail("neither rename landed and x remains")
            raise Fail("x vanished with no dest")
        if x:
            raise Fail("x still exists after a winning rename")
        winner = os.path.join(d, "y" if y else "z")
        eq(rd(winner), b"src", "winner content")

    return [("a", a0), ("ab", (a, b)), ("a", a2)]


@test
def peer_rename_vs_unlink_src():
    """A rename a→b while B unlinks a. Exactly one of a,b exists."""
    def a0(d):
        wr(os.path.join(d, "a"), b"n")
        fsync_path(os.path.join(d, "a"))

    def a(d):
        rc = _race_call("rename a→b", lambda: os.rename(
            os.path.join(d, "a"), os.path.join(d, "b")))
        _race_rc(d, "a", rc)

    def b(d):
        rc = _race_call("unlink a", lambda: os.unlink(os.path.join(d, "a")))
        _race_rc(d, "b", rc)

    def a2(d):
        arc = _read_rc(d, "a")
        brc = _read_rc(d, "b")
        has_a = os.path.exists(os.path.join(d, "a"))
        has_b = os.path.exists(os.path.join(d, "b"))
        if has_a:
            raise Fail("source still present (rename=%s unlink=%s)" % (arc, brc))
        # Exactly one call succeeds. The loser must report ENOENT: a false
        # success that did not touch the name is not a race outcome.
        if (arc == 0) == (brc == 0):
            raise Fail("want exactly one success (rename=%s unlink=%s)" %
                       (arc, brc))
        if arc not in (0, errno.ENOENT) or brc not in (0, errno.ENOENT):
            raise Fail("unexpected errno rename=%s unlink=%s" % (arc, brc))
        if arc == 0:
            if not has_b:
                raise Fail("rename succeeded but b is missing")
            eq(rd(os.path.join(d, "b")), b"n", "renamed content")
        elif has_b:
            raise Fail("b exists after unlink won")

    return [("a", a0), ("ab", (a, b)), ("a", a2)]


@test
def peer_rename_vs_unlink_dst():
    """A rename a→b while B unlinks b. Both calls succeed; a is gone.

    b exists for the whole race: it starts as OLD, and rename replaces it
    atomically, so unlink cannot legitimately observe ENOENT. Afterwards b
    is NEW (unlink ran first) or absent (unlink removed the replacement).
    """
    def a0(d):
        wr(os.path.join(d, "a"), b"NEW")
        wr(os.path.join(d, "b"), b"OLD")
        fsync_path(os.path.join(d, "a"))

    def a(d):
        # Rename has a live source. ENOENT means the source disappeared,
        # which this race does not do — unlink removes the destination.
        try:
            os.rename(os.path.join(d, "a"), os.path.join(d, "b"))
        except OSError as e:
            raise Fail("rename a→b: errno %s (%s)" % (e.errno, e.strerror))
        _race_rc(d, "a", 0)

    def b(d):
        rc = _race_call("unlink b", lambda: os.unlink(os.path.join(d, "b")))
        _race_rc(d, "b", rc)

    def a2(d):
        arc = _read_rc(d, "a")
        brc = _read_rc(d, "b")
        if arc != 0 or brc != 0:
            raise Fail("rename and unlink must both succeed "
                       "(rename=%s unlink=%s)" % (arc, brc))
        if os.path.exists(os.path.join(d, "a")):
            raise Fail("source still present after rename")
        if os.path.exists(os.path.join(d, "b")):
            data = rd(os.path.join(d, "b"))
            if data != b"NEW":
                raise Fail("destination is %r, want NEW or absent" % data)

    return [("a", a0), ("ab", (a, b)), ("a", a2)]


@test
def peer_rename_across_dirs_chase():
    """A rename dir1/x → dir2/x; B rename dir2/x → dir3/x. x exists once."""
    def a0(d):
        os.mkdir(os.path.join(d, "d1"))
        os.mkdir(os.path.join(d, "d2"))
        os.mkdir(os.path.join(d, "d3"))
        wr(os.path.join(d, "d1", "x"), b"moved")
        fsync_path(os.path.join(d, "d1", "x"))

    def a(d):
        try:
            os.rename(os.path.join(d, "d1", "x"), os.path.join(d, "d2", "x"))
        except OSError as e:
            raise Fail("rename d1/x→d2/x: errno %s (%s)" % (e.errno, e.strerror))
        _race_rc(d, "a", 0)

    def b(d):
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            src = os.path.join(d, "d2", "x")
            if os.path.exists(src):
                try:
                    os.rename(src, os.path.join(d, "d3", "x"))
                except OSError as e:
                    if e.errno != errno.ENOENT:
                        raise Fail("chase rename: errno %s (%s)" %
                                   (e.errno, e.strerror))
                    continue
                _race_rc(d, "b", 0)
                return
            time.sleep(0.02)
        # A may have landed the file in d2 after this wait. That is one
        # legal resting place; a2 checks it. A failed chase is not success.
        _race_rc(d, "b", errno.ENOENT)

    def a2(d):
        if _read_rc(d, "a") != 0:
            raise Fail("first rename did not succeed")
        brc = _read_rc(d, "b")
        if os.path.exists(os.path.join(d, "d1", "x")):
            raise Fail("source remains after a successful rename")
        at_d2 = os.path.exists(os.path.join(d, "d2", "x"))
        at_d3 = os.path.exists(os.path.join(d, "d3", "x"))
        if brc == 0:
            if not at_d3 or at_d2:
                raise Fail("B reported a successful chase but x is not only "
                           "at d3 (d2=%s d3=%s)" % (at_d2, at_d3))
            eq(rd(os.path.join(d, "d3", "x")), b"moved", "d3/x")
        elif brc == errno.ENOENT:
            if not at_d2 or at_d3:
                raise Fail("B did not chase but x is not only at d2 "
                           "(d2=%s d3=%s)" % (at_d2, at_d3))
            eq(rd(os.path.join(d, "d2", "x")), b"moved", "d2/x")
        else:
            raise Fail("chase rc %s" % brc)

    return [("a", a0), ("ab", (a, b)), ("a", a2)]


@test
def peer_creat_excl_race():
    """A and B O_CREAT|O_EXCL the same name. Exactly one wins; one EEXIST."""
    def a(d):
        p = os.path.join(d, "foo")
        try:
            fd = os.open(p, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
            os.write(fd, b"A")
            os.close(fd)
            wr(os.path.join(d, ".a-won"), b"1")
        except OSError as e:
            if e.errno != errno.EEXIST:
                raise Fail("A excl errno %s" % e.errno)
            wr(os.path.join(d, ".a-exist"), b"1")

    def b(d):
        p = os.path.join(d, "foo")
        try:
            fd = os.open(p, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
            os.write(fd, b"B")
            os.close(fd)
            wr(os.path.join(d, ".b-won"), b"1")
        except OSError as e:
            if e.errno != errno.EEXIST:
                raise Fail("B excl errno %s" % e.errno)
            wr(os.path.join(d, ".b-exist"), b"1")

    def a2(d):
        a_won = os.path.exists(os.path.join(d, ".a-won"))
        b_won = os.path.exists(os.path.join(d, ".b-won"))
        if a_won == b_won:
            raise Fail("O_EXCL winners a=%s b=%s (want exactly one)" %
                       (a_won, b_won))
        data = rd(os.path.join(d, "foo"))
        if data not in (b"A", b"B"):
            raise Fail("foo content %r" % data)
        eq(os.stat(os.path.join(d, "foo")).st_nlink, 1, "single inode")

    return [("ab", (a, b)), ("a", a2)]


@test
def peer_create_unlink_stat_churn():
    """A create/unlink foo; B open/stat. Never EIO or foreign inode bytes."""
    def a(d):
        p = os.path.join(d, "foo")
        for i in range(40):
            wr(p, ("N%03d" % i).encode())
            try:
                os.unlink(p)
            except OSError as e:
                if e.errno != errno.ENOENT:
                    raise Fail("A unlink during churn: errno %s" % e.errno)

    def b(d):
        p = os.path.join(d, "foo")
        for _ in range(40):
            try:
                st = os.stat(p)
                fd = os.open(p, os.O_RDONLY)
                try:
                    data = os.read(fd, 16)
                finally:
                    os.close(fd)
                if data and not data.startswith(b"N"):
                    raise Fail("B read foreign content %r ino=%s" %
                               (data, st.st_ino))
            except OSError as e:
                if e.errno == errno.EIO:
                    raise Fail("B saw EIO during create/unlink churn")
                if e.errno not in (errno.ENOENT,):
                    raise Fail("B errno %s" % e.errno)

    return [("ab", (a, b))]


@test
def peer_negative_after_unlink():
    """B caches a positive lookup; A unlinks; B sees ENOENT."""
    def a(d):
        wr(os.path.join(d, "foo"), b"x")
        fsync_path(os.path.join(d, "foo"))

    def b1(d):
        eq(rd(os.path.join(d, "foo")), b"x", "B cached positive")

    def a2(d):
        os.unlink(os.path.join(d, "foo"))

    def b2(d):
        try:
            os.stat(os.path.join(d, "foo"))
        except OSError as e:
            if e.errno != errno.ENOENT:
                raise Fail("B after unlink errno %s" % e.errno)
        else:
            raise Fail("B still sees unlinked foo")

    return [("a", a), ("b", b1), ("a", a2), ("b", b2)]


@test
def peer_rename_negative():
    """B caches oldname; A rename old→new; B old ENOENT, new is the object."""
    def a(d):
        wr(os.path.join(d, "old"), b"obj")
        fsync_path(os.path.join(d, "old"))

    def b1(d):
        eq(rd(os.path.join(d, "old")), b"obj", "B cached old")

    def a2(d):
        os.rename(os.path.join(d, "old"), os.path.join(d, "new"))

    def b2(d):
        if os.path.exists(os.path.join(d, "old")):
            raise Fail("B still sees old after rename")
        eq(rd(os.path.join(d, "new")), b"obj", "B new")

    return [("a", a), ("b", b1), ("a", a2), ("b", b2)]


@test
def peer_unlink_recreate_stale_ino():
    """B reads foo; A unlink+create; B must see the new bytes.

    Inode reuse after the last close is allowed, so a repeated st_ino is
    not by itself a stale binding. The old bytes are.
    """
    def a(d):
        wr(os.path.join(d, "foo"), b"one")
        fsync_path(os.path.join(d, "foo"))

    def b1(d):
        wr(os.path.join(d, ".ino1"),
           str(os.stat(os.path.join(d, "foo")).st_ino).encode())
        eq(rd(os.path.join(d, "foo")), b"one", "B first incarnation")

    def a2(d):
        os.unlink(os.path.join(d, "foo"))
        wr(os.path.join(d, "foo"), b"two")
        fsync_path(os.path.join(d, "foo"))

    def b2(d):
        p = os.path.join(d, "foo")
        eq(rd(p), b"two", "B second incarnation content")

    return [("a", a), ("b", b1), ("a", a2), ("b", b2)]


@test
def peer_open_unlink_nlink():
    """A opens foo; B unlinks; A fd still works, nlink 0; B cannot lookup."""
    def a0(d):
        wr(os.path.join(d, "f"), b"live")
        fsync_path(os.path.join(d, "f"))

    def a(d):
        _spawn_holder(d, "holdfd")

    def b(d):
        os.unlink(os.path.join(d, "f"))
        if os.path.exists(os.path.join(d, "f")):
            raise Fail("B still looks up unlinked f")

    def a2(d):
        data, nlink = _holder_go(d)
        eq(data, b"live", "A fd after B unlink")
        eq(nlink, 0, "nlink of the unlinked open file")

    return [("a", a0), ("a", a), ("b", b), ("a", a2)]


@test
def peer_open_rename_fd():
    """A opens foo; B rename foo→bar; A's fd still reads; bar is the object."""
    def a0(d):
        wr(os.path.join(d, "f"), b"fdok")
        fsync_path(os.path.join(d, "f"))

    def a(d):
        _spawn_holder(d, "holdfd")

    def b(d):
        os.rename(os.path.join(d, "f"), os.path.join(d, "bar"))

    def a2(d):
        data, _nlink = _holder_go(d)
        eq(data, b"fdok", "A fd after B rename")
        eq(rd(os.path.join(d, "bar")), b"fdok", "bar")
        if os.path.exists(os.path.join(d, "f")):
            raise Fail("f still present after rename")

    return [("a", a0), ("a", a), ("b", b), ("a", a2)]


@test
def peer_unlink_recreate_old_fd():
    """B unlinks and recreates foo; A's old fd still has the old bytes."""
    def a0(d):
        wr(os.path.join(d, "f"), b"OLDINC")
        fsync_path(os.path.join(d, "f"))

    def a(d):
        _spawn_holder(d, "holdfd")

    def b(d):
        os.unlink(os.path.join(d, "f"))
        wr(os.path.join(d, "f"), b"NEWINC")
        fsync_path(os.path.join(d, "f"))

    def a2(d):
        data, _nlink = _holder_go(d)
        eq(data, b"OLDINC", "old fd is old object")
        eq(rd(os.path.join(d, "f")), b"NEWINC", "name is new object")

    return [("a", a0), ("a", a), ("b", b), ("a", a2)]


@test
def peer_concurrent_hardlink():
    """A link f→a and B link f→b. nlink == 3, both names work."""
    def a0(d):
        wr(os.path.join(d, "f"), b"hl")
        fsync_path(os.path.join(d, "f"))

    def a(d):
        os.link(os.path.join(d, "f"), os.path.join(d, "a"))

    def b(d):
        os.link(os.path.join(d, "f"), os.path.join(d, "b"))

    def a2(d):
        eq(os.stat(os.path.join(d, "f")).st_nlink, 3, "nlink after two links")
        eq(os.stat(os.path.join(d, "a")).st_ino,
           os.stat(os.path.join(d, "b")).st_ino, "same ino")
        eq(rd(os.path.join(d, "a")), b"hl", "a")
        eq(rd(os.path.join(d, "b")), b"hl", "b")

    return [("a", a0), ("ab", (a, b)), ("a", a2)]


@test
def peer_concurrent_unlink_hardlinks():
    """After nlink=3, A unlinks a and B unlinks b. nlink == 1, f remains."""
    def a0(d):
        wr(os.path.join(d, "f"), b"hl")
        os.link(os.path.join(d, "f"), os.path.join(d, "a"))
        os.link(os.path.join(d, "f"), os.path.join(d, "b"))
        fsync_path(os.path.join(d, "f"))

    def a(d):
        os.unlink(os.path.join(d, "a"))

    def b(d):
        os.unlink(os.path.join(d, "b"))

    def a2(d):
        eq(os.stat(os.path.join(d, "f")).st_nlink, 1, "nlink after unlinks")
        eq(rd(os.path.join(d, "f")), b"hl", "f survived")
        if os.path.exists(os.path.join(d, "a")) or os.path.exists(os.path.join(d, "b")):
            raise Fail("a or b still present")

    return [("a", a0), ("ab", (a, b)), ("a", a2)]


@test
def peer_fcntl_range_conflict():
    """A lockf [0,4096); B lockf same range is denied."""
    def a(d):
        wr(os.path.join(d, "f"), b"x" * 8192)
        _spawn_holder(d, "fcntl0")

    def b(d):
        fd = os.open(os.path.join(d, "f"), os.O_RDWR)
        try:
            try:
                fcntl_range(fd, 0, 4096, nb=True)
            except OSError as e:
                if e.errno not in (errno.EAGAIN, errno.EACCES, errno.EWOULDBLOCK):
                    raise Fail("B lockf errno %s" % e.errno)
            else:
                fcntl_range(fd, 0, 4096, unlock=True)
                raise Fail("B acquired overlapping lockf")
        finally:
            os.close(fd)

    def a2(d):
        _holder_go(d)

    return [("a", a), ("b", b), ("a", a2)]


@test
def peer_fcntl_range_adjacent():
    """A lockf [0,4096); B lockf [4096,8192) succeeds."""
    def a(d):
        wr(os.path.join(d, "f"), b"x" * 8192)
        _spawn_holder(d, "fcntl0")

    def b(d):
        fd = os.open(os.path.join(d, "f"), os.O_RDWR)
        try:
            fcntl_range(fd, 4096, 4096, nb=True)
            fcntl_range(fd, 4096, 4096, unlock=True)
        except OSError as e:
            raise Fail("B adjacent lockf failed: %s" % e)
        finally:
            os.close(fd)

    def a2(d):
        _holder_go(d)

    return [("a", a), ("b", b), ("a", a2)]


@test
def peer_mtime_no_regress():
    """A then B write disjoint ranges; mtime must not go backwards."""
    def a(d):
        pwrite_at(os.path.join(d, "f"), b"A" * 4096, 0, creat=True)
        wr(os.path.join(d, ".mt-a"),
           str(os.stat(os.path.join(d, "f")).st_mtime_ns).encode())

    def b(d):
        pwrite_at(os.path.join(d, "f"), b"B" * 4096, 4096)
        wr(os.path.join(d, ".mt-b"),
           str(os.stat(os.path.join(d, "f")).st_mtime_ns).encode())

    def a2(d):
        ma = int(rd(os.path.join(d, ".mt-a")))
        mb = int(rd(os.path.join(d, ".mt-b")))
        now = os.stat(os.path.join(d, "f")).st_mtime_ns
        if mb < ma:
            raise Fail("B mtime %s < A mtime %s" % (mb, ma))
        if now < mb:
            raise Fail("final mtime %s < B mtime %s" % (now, mb))

    return [("a", a), ("b", b), ("a", a2)]


_HOLD_SCRIPT = r"""import ctypes, fcntl, os, sys, time
class _Flock(ctypes.Structure):
    _fields_ = [
        ("l_type", ctypes.c_int16),
        ("l_whence", ctypes.c_int16),
        ("l_start", ctypes.c_int64),
        ("l_len", ctypes.c_int64),
        ("l_pid", ctypes.c_int32),
    ]
mode, path, ready, go, result = sys.argv[1:6]
fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
data = b""
nlink = -1
rc = 2
status = b"FAIL"
err = b"holder failed before ready"
try:
    if mode == "flock":
        fcntl.flock(fd, fcntl.LOCK_EX)
    elif mode == "fcntl0":
        fl = _Flock(l_type=fcntl.F_WRLCK, l_whence=os.SEEK_SET,
                    l_start=0, l_len=4096, l_pid=0)
        fcntl.fcntl(fd, fcntl.F_SETLKW, fl)
    # Publish readiness only after the bytes are durable, via rename, so the
    # peer never observes an empty ready file.
    tmp = ready + ".tmp"
    with open(tmp, "w") as f:
        f.write("1\n")
        f.flush()
        os.fsync(f.fileno())
    os.rename(tmp, ready)
    deadline = time.monotonic() + 20.0
    status = b"FAIL"
    err = b"go timeout"
    while time.monotonic() < deadline:
        try:
            seen = os.path.exists(go)
        except OSError as e:
            err = ("exists %s: errno %s" % (go, e.errno)).encode()
            break
        if seen:
            try:
                if mode == "holdfd":
                    nlink = os.fstat(fd).st_nlink
                    os.lseek(fd, 0, os.SEEK_SET)
                    data = os.read(fd, 4096)
                elif mode == "pwrite1m":
                    os.pwrite(fd, b"Y", 1 << 20)
                    data = b"ok\n"
                else:
                    data = b"ok\n"
            except OSError as e:
                err = ("after go: errno %s" % e.errno).encode()
                break
            status = b"OK"
            err = b""
            rc = 0
            break
        time.sleep(0.05)
finally:
    try:
        os.close(fd)
    except OSError:
        pass
    try:
        tmp = result + ".tmp"
        with open(tmp, "wb") as f:
            f.write(b"%s\t%d\n" % (status, nlink))
            f.write(err if status != b"OK" else data)
            f.flush()
            os.fsync(f.fileno())
        os.rename(tmp, result)
    except OSError:
        pass
sys.exit(rc)
"""


def _holder_paths(d):
    return (os.path.join(d, ".hold-ready"),
            os.path.join(d, ".hold-go"),
            os.path.join(d, ".hold-result"))


def _spawn_holder(d, mode):
    ready, go, result = _holder_paths(d)
    for p in (ready, go, result):
        try:
            os.unlink(p)
        except OSError:
            pass
    script = os.path.join(d, ".hold.py")
    with open(script, "w") as f:
        f.write(_HOLD_SCRIPT)
        f.flush()
        os.fsync(f.fileno())
    target = os.path.join(d, "f")
    subprocess.Popen(
        [sys.executable, script, mode, target, ready, go, result],
        start_new_session=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        if os.path.exists(ready):
            return
        time.sleep(0.05)
    raise Fail("holder %s did not become ready" % mode)


def _parse_holder_result(blob):
    nl = blob.find(b"\n")
    if nl < 0:
        raise Fail("holder result truncated")
    head, payload = blob[:nl], blob[nl + 1:]
    parts = head.split(b"\t")
    if len(parts) != 2 or parts[0] not in (b"OK", b"FAIL"):
        raise Fail("holder result malformed: %r" % head)
    if parts[0] != b"OK":
        raise Fail("holder failed: %s" % payload.decode("utf-8", "replace"))
    return payload, int(parts[1])


def _holder_go(d):
    """Return (payload, nlink). The result file appears only after a full write."""
    _ready, go, result = _holder_paths(d)
    with open(go, "w") as f:
        f.write("1\n")
        f.flush()
        os.fsync(f.fileno())
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        if os.path.exists(result):
            with open(result, "rb") as f:
                return _parse_holder_result(f.read())
        time.sleep(0.05)
    raise Fail("holder produced no result")


def stat_isdir(st):
    return (st.st_mode & 0o170000) == 0o040000


# ==========================================================================
# Prepare / exec / runners
# ==========================================================================

def testdir(mnt, name):
    return os.path.join(mnt, PARENT, name)


def do_prepare(mnt):
    base = os.path.join(mnt, PARENT)
    shutil.rmtree(base, ignore_errors=True)
    try:
        st = os.lstat(base)
    except OSError:
        st = None
    if st is not None and not stat_isdir(st):
        try:
            os.unlink(base)
        except OSError:
            pass
    if os.path.isdir(base):
        for n in os.listdir(base):
            p = os.path.join(base, n)
            shutil.rmtree(p, ignore_errors=True)
            try:
                os.unlink(p)
            except OSError:
                pass
    try:
        os.makedirs(base, exist_ok=True)
    except FileExistsError:
        # Server still has the name (rmtree missed a leftover) but a
        # post-remount getattr did not see a directory. Reuse it.
        pass
    for name, _steps, _doc in TESTS:
        os.makedirs(os.path.join(base, name), exist_ok=True)
    # Commit so a remounted peer adopts these inos (GET_META is last
    # commit, not unflushed RAM). Without this, B remounts the previous
    # posix-2c and LOOKUP walks the empty old dir.
    keep = os.path.join(base, ".keep")
    fd = os.open(keep, os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
    try:
        os.write(fd, b"x")
        os.fsync(fd)
    finally:
        os.close(fd)
    print("prepared %s (%d testdirs)" % (base, len(TESTS)))


def run_step_index(name, idx, d, ab_side=None):
    steps = None
    for n, s, _doc in TESTS:
        if n == name:
            steps = s
            break
    if steps is None:
        raise Fail("unknown test %s" % name)
    if idx < 0 or idx >= len(steps):
        raise Fail("bad step index %d for %s" % (idx, name))
    side, fn = steps[idx]
    if side == "ab":
        fa, fb = fn
        if ab_side == "b":
            fb(d)
        else:
            fa(d)
    else:
        fn(d)


def write_results(path, host_a, host_b, mnt, npass, nfail, dt, quiet=False,
                  selected=None, done=False):
    """Write the TSV. `# complete` is only for a finished selection.

    Remote runs rewrite this file after every test. Those snapshots list
    the full selected inventory but omit the marker, so a prefix of PASS
    rows is not a successful comparison.
    """
    selected = list(selected or [])
    have = set(name for name, _res, _detail in RESULTS)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write("# posix-2client host_a=%s host_b=%s mnt=%s %s\n" %
                (host_a, host_b, mnt,
                 time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())))
        for name in selected:
            f.write("# select\t%s\n" % name)
        f.write("test\tresult\tdetail\n")
        for name, res, detail in RESULTS:
            f.write("%s\t%s\t%s\n" %
                    (name, res, (detail or "").replace("\n", " ")))
        f.write("# summary pass=%d fail=%d skip=0 total=%d dur=%.1f\n" %
                (npass, nfail, npass + nfail, dt))
        if done and selected and set(selected) <= have:
            f.write("# complete\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    if not quiet:
        print("wrote %s" % path, flush=True)


def _matched(filt):
    return [t for t in TESTS if not filt or filt in t[0]]


def run_local(mnt_a, mnt_b, results_file, filt, keep):
    if not _matched(filt):
        print("ERROR: no tests matched filter %r" % (filt,))
        return 2
    do_prepare(mnt_a)
    # same-path XFS: testdirs exist on B immediately. Two FUSE mounts:
    # B must already see PARENT (harness remounted B after a prior prepare,
    # or we just prepared on A and B is the same path).
    parent_b = os.path.join(mnt_b, PARENT)
    if not os.path.isdir(parent_b):
        print("ERROR: %s missing on B — remount B after --prepare" % parent_b)
        return 2

    selected = [t[0] for t in _matched(filt)]
    npass = nfail = 0
    t0 = time.time()
    try:
        for name, steps, _doc in TESTS:
            if filt and filt not in name:
                continue
            da = testdir(mnt_a, name)
            db = testdir(mnt_b, name)
            if not os.path.isdir(db):
                nfail += 1
                RESULTS.append((name, "FAIL",
                                "testdir missing on B (stale snapshot?)"))
                print("FAIL %-32s testdir missing on B" % name, flush=True)
                continue
            try:
                for side, fn in steps:
                    if side == "ab":
                        fa, fb = fn
                        _run_pair(fa, da, fb, db)
                    else:
                        fn(da if side == "a" else db)
            except Fail as e:
                nfail += 1
                RESULTS.append((name, "FAIL", str(e)))
                print("FAIL %-32s %s" % (name, e), flush=True)
            except Exception as e:  # noqa: BLE001
                nfail += 1
                RESULTS.append((name, "FAIL", "%s: %s" % (type(e).__name__, e)))
                print("FAIL %-32s %s: %s" % (name, type(e).__name__, e), flush=True)
            else:
                npass += 1
                RESULTS.append((name, "PASS", ""))
                print("pass %-32s" % name, flush=True)
    finally:
        if not keep:
            shutil.rmtree(os.path.join(mnt_a, PARENT), ignore_errors=True)

    dt = time.time() - t0
    total = npass + nfail
    print("=" * 60)
    print("POSIX 2-client: %d/%d pass, %d fail  (%.1fs)" %
          (npass, total, nfail, dt))
    if results_file:
        write_results(results_file, "local-a", "local-b", mnt_a,
                      npass, nfail, dt, selected=selected, done=True)
    return 0 if nfail == 0 else 1


def _check_exec_result(side, i, rc, out, err):
    """A RESULT line is not success unless the process also exited 0.

    A step can print PASS and then hang until the SSH deadline kills it.
    That deadline must stay a failure. More than one RESULT row is malformed.
    """
    if rc != 0:
        raise Fail("exec %s step %d rc=%d out=%r err=%r" %
                   (side, i, rc, out[-300:], err[-300:]))
    lines = [ln for ln in out.splitlines() if ln.startswith("RESULT\t")]
    if len(lines) != 1:
        raise Fail("exec %s step %d: %d RESULT rows out=%r err=%r" %
                   (side, i, len(lines), out[-300:], err[-300:]))
    parts = lines[0].split("\t", 2)
    status = parts[1] if len(parts) > 1 else "?"
    detail = parts[2] if len(parts) > 2 else ""
    if status != "PASS":
        raise Fail(detail or status)


def _remote_ab(ssh, script, host_a, host_b, name, idx, d):
    results = [None, None]

    def one(slot, host, ab_side):
        cmd = "python3 %s --exec %s %d %s %s" % (
            _q(script), _q(name), idx, _q(d), ab_side)
        results[slot] = ssh_cmd(ssh, host, cmd)

    ta = threading.Thread(target=one, args=(0, host_a, "a"))
    tb = threading.Thread(target=one, args=(1, host_b, "b"))
    ta.start()
    tb.start()
    ta.join()
    tb.join()
    for slot, label in ((0, "a"), (1, "b")):
        rc, out, err = results[slot]
        _check_exec_result("ab-%s" % label, idx, rc, out, err)


def ssh_cmd(ssh, host, remote):
    sec = int(os.environ.get("POSIX2_STEP_SEC", os.environ.get(
        "EFS_SSH_TIMEOUT", "15")))
    env = os.environ.copy()
    env["EFS_SSH_TIMEOUT"] = str(sec)
    r = subprocess.run([ssh, host, remote],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       env=env)
    out = r.stdout.decode("utf-8", "replace")
    err = r.stderr.decode("utf-8", "replace")
    if r.returncode == 124:
        err = (err + " ssh timeout after %ss" % sec).strip()
    return r.returncode, out, err


def run_remote(host_a, host_b, mnt, ssh, script, results_file, filt):
    if not _matched(filt):
        print("ERROR: no tests matched filter %r" % (filt,))
        return 2
    npass = nfail = 0
    t0 = time.time()
    parent_b = os.path.join(mnt, PARENT)
    rc, out, err = ssh_cmd(
        ssh, host_b,
        "test -d %s && echo OK || echo MISSING" % _q(parent_b))
    if "OK" not in out:
        print("ERROR: %s not visible on %s — remount B after --prepare" %
              (parent_b, host_b))
        if err:
            print(err)
        return 2

    selected = [t[0] for t in _matched(filt)]
    for name, steps, _doc in TESTS:
        if filt and filt not in name:
            continue
        d = testdir(mnt, name)
        try:
            for i, (side, _fn) in enumerate(steps):
                tstep = time.time()
                try:
                    if side == "ab":
                        _remote_ab(ssh, script, host_a, host_b, name, i, d)
                    else:
                        host = host_a if side == "a" else host_b
                        cmd = "python3 %s --exec %s %d %s" % (
                            _q(script), _q(name), i, _q(d))
                        rc, out, err = ssh_cmd(ssh, host, cmd)
                        _check_exec_result(side, i, rc, out, err)
                finally:
                    el = time.time() - tstep
                    if el >= 2.0:
                        print("slow-step %s %s %d %.2fs" %
                              (name, side, i, el), flush=True)
        except Fail as e:
            nfail += 1
            RESULTS.append((name, "FAIL", str(e)))
            print("FAIL %-32s %s" % (name, e), flush=True)
        else:
            npass += 1
            RESULTS.append((name, "PASS", ""))
            print("pass %-32s" % name, flush=True)
        if results_file:
            write_results(results_file, host_a, host_b, mnt, npass, nfail,
                          time.time() - t0, quiet=True, selected=selected,
                          done=False)

    dt = time.time() - t0
    total = npass + nfail
    print("=" * 60)
    print("POSIX 2-client: %d/%d pass, %d fail  (%.1fs)" %
          (npass, total, nfail, dt))
    if results_file:
        write_results(results_file, host_a, host_b, mnt, npass, nfail, dt,
                      selected=selected, done=True)
    return 0 if nfail == 0 else 1


def _q(s):
    return "'" + s.replace("'", "'\\''") + "'"


def _step_fns(name):
    for n, steps, _doc in TESTS:
        if n == name:
            return steps
    raise AssertionError(name)


def _self_test():
    fails = []

    def must_fail(label, fn):
        try:
            fn()
        except Fail:
            return
        fails.append(label + " was accepted")

    def must_pass(label, fn):
        try:
            fn()
        except Fail as e:
            fails.append("%s: %s" % (label, e))

    must_pass("rc0 PASS", lambda: _check_exec_result(
        "a", 0, 0, "RESULT\tPASS\t\n", ""))
    for rc in (124, 255, -15):
        must_fail("PASS with rc=%s" % rc, lambda rc=rc: _check_exec_result(
            "a", 0, rc, "RESULT\tPASS\t\n", ""))
    must_fail("missing RESULT", lambda: _check_exec_result("a", 0, 0, "", ""))
    must_fail("two RESULT rows", lambda: _check_exec_result(
        "a", 0, 0, "RESULT\tPASS\t\nRESULT\tFAIL\tx\n", ""))
    must_fail("RESULT FAIL", lambda: _check_exec_result(
        "a", 0, 0, "RESULT\tFAIL\tx\n", ""))

    real_rename, real_unlink = os.rename, os.unlink

    def boom(*_a, **_k):
        raise OSError(errno.EIO, "injected")

    def noop(*_a, **_k):
        return None

    def exercise(name, setup_idx, ab_idx, check_idx):
        steps = _step_fns(name)
        setup, (fa, fb), check = (steps[setup_idx][1], steps[ab_idx][1],
                                  steps[check_idx][1])
        for label, repl in (("eio", boom), ("noop", noop)):
            d = tempfile.mkdtemp(prefix="posix2-race-")
            try:
                setup(d)
                os.rename = repl
                os.unlink = repl
                try:
                    if label == "eio":
                        must_fail("%s %s side a" % (name, label), lambda: fa(d))
                        must_fail("%s %s side b" % (name, label), lambda: fb(d))
                    else:
                        fa(d)
                        fb(d)
                        must_fail("%s noop final" % name, lambda: check(d))
                finally:
                    os.rename, os.unlink = real_rename, real_unlink
            finally:
                shutil.rmtree(d, ignore_errors=True)
        # Both legal serial orders, with the real syscalls.
        for order in ("ab", "ba"):
            d = tempfile.mkdtemp(prefix="posix2-order-")
            try:
                setup(d)
                if order == "ab":
                    fa(d)
                    fb(d)
                else:
                    fb(d)
                    fa(d)
                must_pass("%s order %s" % (name, order), lambda: check(d))
            finally:
                shutil.rmtree(d, ignore_errors=True)

    exercise("peer_rename_vs_unlink_src", 0, 1, 2)
    exercise("peer_rename_vs_unlink_dst", 0, 1, 2)
    exercise("peer_rename_same_src_two_dst", 0, 1, 2)

    def lie_ok(*_a, **_k):
        return None

    def lie_enoent(*_a, **_k):
        raise OSError(errno.ENOENT, "injected")

    def run_sides(name, rename_fn, unlink_fn, order="ab"):
        steps = _step_fns(name)
        setup, (fa, fb), check = steps[0][1], steps[1][1], steps[2][1]
        d = tempfile.mkdtemp(prefix="posix2-side-")
        try:
            setup(d)
            os.rename = rename_fn
            os.unlink = unlink_fn
            try:
                if order == "ab":
                    fa(d)
                    fb(d)
                else:
                    fb(d)
                    fa(d)
            finally:
                os.rename, os.unlink = real_rename, real_unlink
            check(d)
        finally:
            shutil.rmtree(d, ignore_errors=True)

    must_fail("src false unlink success", lambda: run_sides(
        "peer_rename_vs_unlink_src", real_rename, lie_ok))
    must_fail("src false rename success", lambda: run_sides(
        "peer_rename_vs_unlink_src", lie_ok, real_unlink))
    must_fail("src rename EIO", lambda: run_sides(
        "peer_rename_vs_unlink_src", boom, real_unlink))
    must_fail("src unlink EIO", lambda: run_sides(
        "peer_rename_vs_unlink_src", real_rename, boom))
    must_fail("dst unlink false ENOENT", lambda: run_sides(
        "peer_rename_vs_unlink_dst", real_rename, lie_enoent))
    must_fail("dst rename EIO", lambda: run_sides(
        "peer_rename_vs_unlink_dst", boom, real_unlink))
    must_fail("dst unlink EIO", lambda: run_sides(
        "peer_rename_vs_unlink_dst", real_rename, boom))

    chase = _step_fns("peer_rename_across_dirs_chase")
    d = tempfile.mkdtemp(prefix="posix2-chase-")
    try:
        chase[0][1](d)
        rename_step = chase[1][1][0]
        os.rename = boom
        try:
            must_fail("chase EIO", lambda: rename_step(d))
        finally:
            os.rename = real_rename
        os.rename = noop
        try:
            rename_step(d)
            must_fail("chase noop", lambda: chase[2][1](d))
        finally:
            os.rename = real_rename
        # A really moves d1/x to d2/x. B claims the second rename succeeded
        # and does not move it. That acknowledgement is not a legal miss.
        os.rename = real_rename
        rename_step(d)
        os.rename = noop
        try:
            chase[1][1][1](d)
            must_fail("chase false success", lambda: chase[2][1](d))
        finally:
            os.rename = real_rename
    finally:
        shutil.rmtree(d, ignore_errors=True)
        os.rename = real_rename

    d = tempfile.mkdtemp(prefix="posix2-hold-")
    try:
        wr(os.path.join(d, "f"), b"live")
        _spawn_holder(d, "holdfd")
        os.unlink(os.path.join(d, "f"))
        data, nlink = _holder_go(d)
        if data != b"live" or nlink != 0:
            fails.append("holder after unlink data=%r nlink=%s" % (data, nlink))
    except Fail as e:
        fails.append("holder: %s" % e)
    finally:
        shutil.rmtree(d, ignore_errors=True)

    mnt = tempfile.mkdtemp(prefix="posix2-filt-")
    try:
        os.makedirs(os.path.join(mnt, PARENT))
        marker = os.path.join(mnt, PARENT, "keep")
        with open(marker, "w") as f:
            f.write("stay")
        rc = run_local(mnt, mnt, None, "definitely_missing_test", True)
        if rc != 2:
            fails.append("unmatched filter rc=%s" % rc)
        if not os.path.isfile(marker) or open(marker).read() != "stay":
            fails.append("unmatched filter changed existing prepare data")
    finally:
        shutil.rmtree(mnt, ignore_errors=True)

    import compare
    mnt = tempfile.mkdtemp(prefix="posix2-cmp-")
    try:
        r1 = os.path.join(mnt, "a.tsv")
        r2 = os.path.join(mnt, "b.tsv")
        run = subprocess.run(
            [sys.executable, os.path.abspath(__file__), "--local", mnt, mnt,
             "--filter", "peer_create_visible", "--results", r1, "--keep"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if run.returncode != 0:
            fails.append("peer_create_visible CLI rc=%s" % run.returncode)
        else:
            code, lines = compare.compare_files(r1, r1)
            if code != 0:
                fails.append("two-client results file does not compare: %s" %
                             "\n".join(lines[-8:]))
        run = subprocess.run(
            [sys.executable, os.path.abspath(__file__), "--local", mnt, mnt,
             "--filter", "peer_mkdir_visible", "--results", r2, "--keep"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if run.returncode != 0:
            fails.append("peer_mkdir_visible CLI rc=%s" % run.returncode)
        else:
            code, lines = compare.compare_files(r1, r2)
            text = "\n".join(lines)
            if code == 0 or "inventory differs" not in text:
                fails.append("filtered inventories compared as the same run")
        snap = os.path.join(mnt, "snap.tsv")
        RESULTS.append(("peer_create_visible", "PASS", ""))
        try:
            write_results(snap, "a", "b", mnt, 1, 0, 0.1, quiet=True,
                          selected=[t[0] for t in _matched("peer_")],
                          done=False)
            code, lines = compare.compare_files(snap, snap)
            text = "\n".join(lines)
            if code == 0 or "no # complete" not in text:
                fails.append("remote snapshot was treated as finished")
        finally:
            if RESULTS and RESULTS[-1][0] == "peer_create_visible":
                RESULTS.pop()
    finally:
        shutil.rmtree(mnt, ignore_errors=True)

    if fails:
        for msg in fails:
            print("FAIL " + msg)
        return 1
    print("posix_2client self-test: pass")
    return 0


def main(argv):
    global PARENT
    args = list(argv)
    if args == ["--self-test"]:
        return _self_test()
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 2

    results_file = None
    filt = None
    keep = False
    ssh = os.environ.get(
        "EFS_SSH",
        os.path.expanduser("~/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh"))
    mnt = None
    i = 0
    # pre-scan common flags (order-independent enough for our callers)
    rest = []
    while i < len(args):
        if args[i] == "--results":
            results_file = args[i + 1]
            i += 2
        elif args[i] == "--filter":
            filt = args[i + 1]
            i += 2
        elif args[i] == "--keep":
            keep = True
            i += 1
        elif args[i] == "--parent":
            PARENT = args[i + 1]
            os.environ["EFS_POSIX2_PARENT"] = PARENT
            i += 2
        elif args[i] == "--mnt":
            mnt = args[i + 1]
            i += 2
        elif args[i] == "--ssh":
            ssh = args[i + 1]
            i += 2
        else:
            rest.append(args[i])
            i += 1
    args = rest

    cmd = args[0]
    if cmd == "--prepare":
        if len(args) < 2:
            print("usage: --prepare <mnt>")
            return 2
        do_prepare(args[1])
        return 0

    if cmd == "--exec":
        # --exec <test> <step-index> <dir> [a|b]
        if len(args) < 4:
            print("usage: --exec <test> <step-index> <dir> [a|b]")
            return 2
        name, idx_s, d = args[1], args[2], args[3]
        ab_side = args[4] if len(args) >= 5 else None
        try:
            run_step_index(name, int(idx_s), d, ab_side)
        except Fail as e:
            print("RESULT\tFAIL\t%s" % e)
            return 1
        except Exception as e:  # noqa: BLE001
            print("RESULT\tFAIL\t%s: %s" % (type(e).__name__, e))
            return 1
        print("RESULT\tPASS\t")
        return 0

    if cmd == "--local":
        if len(args) < 3:
            print("usage: --local <mnt-a> <mnt-b>")
            return 2
        return run_local(args[1], args[2], results_file, filt, keep)

    if cmd == "--remote":
        if len(args) < 3 or not mnt:
            print("usage: --remote <host-a> <host-b> --mnt <mnt>")
            return 2
        script = os.path.abspath(__file__)
        return run_remote(args[1], args[2], mnt, ssh, script,
                          results_file, filt)

    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
