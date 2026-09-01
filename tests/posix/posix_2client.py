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

import errno
import fcntl
import os
import shutil
import subprocess
import sys
import threading
import time

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
    """IOR-hard write: A and B pwrite distinct ranges of one file; both read."""
    def a(d):
        fd = os.open(os.path.join(d, "file"), os.O_CREAT | os.O_RDWR, 0o644)
        os.pwrite(fd, b"A" * 4096, 0)
        os.close(fd)

    def b(d):
        p = os.path.join(d, "file")
        try:
            fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
        except OSError as e:
            raise Fail("B open shared file: %s (errno %s)" %
                       (e.strerror, e.errno))
        os.pwrite(fd, b"B" * 4096, 4096)
        os.close(fd)

    def a2(d):
        p = os.path.join(d, "file")
        with open(p, "rb") as f:
            data = f.read(8192)
        if data[:4096] != b"A" * 4096:
            raise Fail("A no longer has its own 4k at offset 0")
        if data[4096:8192] != b"B" * 4096:
            raise Fail("A cannot read B's 4k at offset 4096 (got %r)" %
                       data[4096:4112])

    return [("a", a), ("b", b), ("a", a2)]


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
        data = _holder_go(d)
        eq(data, b"still-here", "B fd read after A unlink")

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
            try:
                os.truncate(p, 0)
            except OSError:
                pass
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
        try:
            os.rename(os.path.join(d, "x"), os.path.join(d, "y"))
        except OSError:
            pass

    def b(d):
        try:
            os.rename(os.path.join(d, "x"), os.path.join(d, "z"))
        except OSError:
            pass

    def a2(d):
        y = os.path.exists(os.path.join(d, "y"))
        z = os.path.exists(os.path.join(d, "z"))
        x = os.path.exists(os.path.join(d, "x"))
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
        try:
            os.rename(os.path.join(d, "a"), os.path.join(d, "b"))
        except OSError:
            pass

    def b(d):
        try:
            os.unlink(os.path.join(d, "a"))
        except OSError:
            pass

    def a2(d):
        has_a = os.path.exists(os.path.join(d, "a"))
        has_b = os.path.exists(os.path.join(d, "b"))
        if has_a and has_b:
            raise Fail("both a and b exist")
        if not has_a and not has_b:
            return
        if has_b:
            eq(rd(os.path.join(d, "b")), b"n", "renamed content")

    return [("a", a0), ("ab", (a, b)), ("a", a2)]


@test
def peer_rename_vs_unlink_dst():
    """A rename a→b while B unlinks b. Legal: a gone, b is a or missing."""
    def a0(d):
        wr(os.path.join(d, "a"), b"NEW")
        wr(os.path.join(d, "b"), b"OLD")
        fsync_path(os.path.join(d, "a"))

    def a(d):
        try:
            os.rename(os.path.join(d, "a"), os.path.join(d, "b"))
        except OSError:
            pass

    def b(d):
        try:
            os.unlink(os.path.join(d, "b"))
        except OSError:
            pass

    def a2(d):
        has_a = os.path.exists(os.path.join(d, "a"))
        has_b = os.path.exists(os.path.join(d, "b"))
        if has_a and has_b:
            if rd(os.path.join(d, "b")) == b"NEW":
                raise Fail("a still exists after rename-over")
        if has_b:
            data = rd(os.path.join(d, "b"))
            if data not in (b"NEW", b"OLD"):
                raise Fail("b has corrupt content %r" % data)

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
        except OSError:
            pass

    def b(d):
        deadline = time.time() + 5.0
        while time.time() < deadline:
            src = os.path.join(d, "d2", "x")
            if os.path.exists(src):
                try:
                    os.rename(src, os.path.join(d, "d3", "x"))
                    return
                except OSError:
                    return
            time.sleep(0.05)

    def a2(d):
        hits = []
        for name in ("d1/x", "d2/x", "d3/x"):
            p = os.path.join(d, name)
            if os.path.exists(p):
                eq(rd(p), b"moved", name)
                hits.append(name)
        if len(hits) != 1:
            raise Fail("x at %s (want exactly one)" % hits)

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
            try:
                wr(p, ("N%03d" % i).encode())
                os.unlink(p)
            except OSError:
                pass

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
    """B stats foo (ino1); A unlink+create foo (ino2); B must see ino2/content."""
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
        ino1 = int(rd(os.path.join(d, ".ino1")))
        ino2 = os.stat(p).st_ino
        if ino2 == ino1:
            raise Fail("B still bound to inode %s after recreate" % ino1)

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
        data = _holder_go(d)
        eq(data, b"live", "A fd after B unlink")

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
        data = _holder_go(d)
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
        data = _holder_go(d)
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
                fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB, 4096)
            except OSError as e:
                if e.errno not in (errno.EAGAIN, errno.EACCES, errno.EWOULDBLOCK):
                    raise Fail("B lockf errno %s" % e.errno)
            else:
                fcntl.lockf(fd, fcntl.LOCK_UN, 4096)
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
            os.lseek(fd, 4096, os.SEEK_SET)
            fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB, 4096)
            fcntl.lockf(fd, fcntl.LOCK_UN, 4096)
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


_HOLD_SCRIPT = r"""import fcntl, os, sys, time
mode, path, ready, go, result = sys.argv[1:6]
fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
data = b""
rc = 2
try:
    if mode == "flock":
        fcntl.flock(fd, fcntl.LOCK_EX)
    elif mode == "fcntl0":
        fcntl.lockf(fd, fcntl.LOCK_EX, 4096)
    with open(ready, "w") as f:
        f.write("1\n")
        f.flush()
        os.fsync(f.fileno())
    deadline = time.time() + 20.0
    while time.time() < deadline:
        if os.path.exists(go):
            if mode == "holdfd":
                os.lseek(fd, 0, os.SEEK_SET)
                data = os.read(fd, 4096)
            elif mode == "pwrite1m":
                os.pwrite(fd, b"Y", 1 << 20)
                data = b"ok\n"
            else:
                data = b"ok\n"
            rc = 0
            break
        time.sleep(0.05)
finally:
    try:
        os.close(fd)
    except OSError:
        pass
    try:
        with open(result, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
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
    script = "/tmp/efs-posix2-hold.py"
    with open(script, "w") as f:
        f.write(_HOLD_SCRIPT)
    target = os.path.join(d, "f")
    subprocess.Popen(
        [sys.executable, script, mode, target, ready, go, result],
        start_new_session=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    deadline = time.time() + 10.0
    while time.time() < deadline:
        if os.path.exists(ready):
            return
        time.sleep(0.05)
    raise Fail("holder %s did not become ready" % mode)


def _holder_go(d):
    ready, go, result = _holder_paths(d)
    with open(go, "w") as f:
        f.write("1\n")
        f.flush()
        os.fsync(f.fileno())
    deadline = time.time() + 8.0
    while time.time() < deadline:
        if os.path.exists(result):
            with open(result, "rb") as f:
                return f.read()
        if not os.path.exists(ready):
            break
        time.sleep(0.05)
    if os.path.exists(result):
        with open(result, "rb") as f:
            return f.read()
    return b""


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


def write_results(path, host_a, host_b, mnt, npass, nfail, dt, quiet=False):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write("# posix-2client host_a=%s host_b=%s mnt=%s %s\n" %
                (host_a, host_b, mnt,
                 time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())))
        f.write("test\tresult\tdetail\n")
        for name, res, detail in RESULTS:
            f.write("%s\t%s\t%s\n" %
                    (name, res, detail.replace("\n", " ")))
        f.write("# summary pass=%d fail=%d skip=0 total=%d dur=%.1f\n" %
                (npass, nfail, npass + nfail, dt))
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    if not quiet:
        print("wrote %s" % path, flush=True)


def run_local(mnt_a, mnt_b, results_file, filt, keep):
    do_prepare(mnt_a)
    # same-path XFS: testdirs exist on B immediately. Two FUSE mounts:
    # B must already see PARENT (harness remounted B after a prior prepare,
    # or we just prepared on A and B is the same path).
    parent_b = os.path.join(mnt_b, PARENT)
    if not os.path.isdir(parent_b):
        print("ERROR: %s missing on B — remount B after --prepare" % parent_b)
        return 2

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
                      npass, nfail, dt)
    return 0 if nfail == 0 else 1


def _check_exec_result(side, i, rc, out, err):
    line = ""
    for ln in out.splitlines():
        if ln.startswith("RESULT\t"):
            line = ln
    if not line:
        raise Fail("no RESULT from %s step %d: rc=%d out=%r err=%r" %
                   (side, i, rc, out[-300:], err[-300:]))
    parts = line.split("\t", 2)
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

    for name, steps, _doc in TESTS:
        if filt and filt not in name:
            continue
        d = testdir(mnt, name)
        try:
            for i, (side, _fn) in enumerate(steps):
                if side == "ab":
                    _remote_ab(ssh, script, host_a, host_b, name, i, d)
                    continue
                host = host_a if side == "a" else host_b
                cmd = "python3 %s --exec %s %d %s" % (
                    _q(script), _q(name), i, _q(d))
                rc, out, err = ssh_cmd(ssh, host, cmd)
                _check_exec_result(side, i, rc, out, err)
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
                          time.time() - t0, quiet=True)

    dt = time.time() - t0
    total = npass + nfail
    print("=" * 60)
    print("POSIX 2-client: %d/%d pass, %d fail  (%.1fs)" %
          (npass, total, nfail, dt))
    if results_file:
        write_results(results_file, host_a, host_b, mnt, npass, nfail, dt)
    return 0 if nfail == 0 else 1


def _q(s):
    return "'" + s.replace("'", "'\\''") + "'"


def main(argv):
    global PARENT
    args = list(argv)
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
