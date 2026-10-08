#!/usr/bin/env python3
"""Comprehensive POSIX correctness suite for an efs FUSE mount.

Reusable + extensible: each test is a function decorated with @test that
receives a fresh empty directory inside the mount. Add a new test by writing
a function and decorating it — the runner picks it up automatically.
A test decorated with @root instead receives the mount root itself (the
export root is a sharded directory whose metadata placement differs from
nested dirs — W56); it must use unique entry names and clean up after
itself.

Covers both "possible" operations (must succeed) and "impossible" ones
(must fail with a specific errno), using a mix of direct syscalls and real
terminal commands (mkdir, ln, dd, cp, mv, stat, ...) via subprocess.

Usage:
    posix_suite.py <mount-dir> [--results <file>] [--keep] [--stop]
                   [--filter <substr>] [--timeout-s <sec>] [--jobs N]

Exit code: 0 if every selected test passes, 1 otherwise.

Two-client visibility (create on A, see on B without remount) lives in
posix_2client.py — run via `tests/run_tests.sh posix2`.
"""
import errno
import fcntl
import hashlib
import mmap
import os
import shutil
import signal
import stat as statmod
import subprocess
import sys
import tempfile
import threading
import time

# --------------------------------------------------------------------------
# Test registry + result tracking
# --------------------------------------------------------------------------
TESTS = []          # list of (name, fn)
RESULTS = []        # list of (name, ok, detail)


def test(fn):
    """Register a test function. The function name becomes the test id."""
    TESTS.append((fn.__name__, fn))
    return fn


def serial(fn):
    """Process-global umask or cwd — runner will not overlap this test."""
    fn._posix_serial = True
    return fn


def budget(sec):
    """Per-test wall, when 300 create+unlink cannot fit the default 15 s."""
    def deco(fn):
        fn._posix_timeout = int(sec)
        return fn
    return deco


def root(fn):
    """Test receives the MOUNT ROOT instead of a fresh scratch testdir, for
    behavior that differs between the export root and nested directories
    (the sharded export places root dentries on different metadata shards —
    W56's root-level rename ghost). Entries must have unique names (the
    mount may be shared with other runs) and be cleaned up by the test."""
    fn._posix_root = True
    return fn


class Fail(Exception):
    """Raise to fail a test with a message. soft=True marks it SKIP (feature
    unsupported) rather than a hard failure."""

    def __init__(self, msg, soft=False):
        super().__init__(msg)
        self.soft = soft


class TestTimeout(Fail):
    """One test exceeded --timeout-s. D-state FUSE may still ignore SIGALRM;
    the remote SSH deadline in run_tests.sh is the outer backstop."""

    def __init__(self, sec):
        Fail.__init__(self, "timeout after %ss" % sec)


def expect_err(want, fn, *a, **kw):
    """Run fn expecting it to raise OSError with one of the given errnos.
    `want` is a single errno or a tuple of acceptable errnos."""
    wantset = want if isinstance(want, tuple) else (want,)
    try:
        fn(*a, **kw)
    except OSError as e:
        if e.errno in wantset:
            return
        raise Fail("wrong errno: got %s (%s), want one of %s" %
                   (e.errno, e.strerror, wantset))
    raise Fail("expected errno %s but call succeeded" % (wantset,))


def sh(cmd, cwd=None, ok=True):
    """Run a terminal command (list or str). Returns CompletedProcess.
    ok=True asserts rc==0; ok=False asserts rc!=0."""
    r = subprocess.run(cmd, cwd=cwd, shell=isinstance(cmd, str),
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if ok and r.returncode != 0:
        raise Fail("cmd %r failed rc=%d: %s" % (cmd, r.returncode,
                                                r.stderr.decode('utf-8', 'replace')))
    if not ok and r.returncode == 0:
        raise Fail("cmd %r unexpectedly succeeded" % (cmd,))
    return r


def wr(path, data):
    with open(path, "wb") as f:
        f.write(data)


def rd(path):
    with open(path, "rb") as f:
        return f.read()


def eq(got, want, what=""):
    if got != want:
        raise Fail("%s: got %r, want %r" % (what, got, want))


# Seeded content generator: reproducible mixed ascii/binary data (NOT zeros or
# a repeated char) so zero-fill / dedup / repetitive-data bugs can't hide. A
# SHA-256 counter keystream is mapped through _MIX_TABLE for a realistic blend.
def _build_mix_table():
    t = bytearray(256)
    for r in range(256):
        if r < 108:                    # ~42% printable ASCII (0x20..0x7e)
            t[r] = 0x20 + (r % 0x5f)
        elif r < 132:                  # ~9% whitespace (space, \t, \n, \r)
            t[r] = (0x20, 0x09, 0x0a, 0x0d)[r % 4]
        elif r < 152:                  # ~8% control bytes (0x01..0x1f)
            t[r] = 0x01 + (r % 0x1f)
        elif r < 162:                  # ~4% NUL
            t[r] = 0x00
        else:                          # ~37% high / binary bytes (0x80..0xff)
            t[r] = r
    return bytes(t)


_MIX_TABLE = _build_mix_table()


def rand_bytes(seed, n):
    """`n` reproducible pseudo-random bytes from `seed` (str/int/bytes): a
    deliberate mix of printable ASCII, spaces/whitespace, control chars, NULs,
    and high/binary bytes. Same seed -> same bytes on any client, so a peer can
    verify content it did not write. Deterministic across Python versions
    (SHA-256 counter, not the Mersenne RNG)."""
    if isinstance(seed, str):
        seed = seed.encode("utf-8")
    elif isinstance(seed, int):
        seed = str(seed).encode("ascii")
    out = bytearray()
    ctr = 0
    while len(out) < n:
        out += hashlib.sha256(seed + ctr.to_bytes(4, "big")).digest()
        ctr += 1
    return bytes(out[:n]).translate(_MIX_TABLE)


def rmdir_rideout_sillyrename(p, tries=200, delay=0.005):
    """os.rmdir, riding out the FUSE silly-rename transient. Unlinking a file
    whose close-release the kernel has deferred leaves a .fuse_hidden dentry
    until libfuse's release lands, so rmdir transiently reports ENOTEMPTY
    (~100us idle, but longer under the suite's concurrent open/close load —
    beyond the daemon's own 20ms retry). XFS has no silly-rename, so the
    baseline passes immediately; retry briefly to compare like-for-like. A
    genuinely non-empty dir still fails, just after the bounded delay."""
    for _ in range(tries):
        try:
            os.rmdir(p)
            return
        except OSError as e:
            if e.errno != errno.ENOTEMPTY:
                raise
            time.sleep(delay)
    os.rmdir(p)  # final attempt: raises if still (genuinely) non-empty


def close_leaked_mount_fds(mnt):
    """Drop FUSE fds a test leaked so suite exit is not a close storm."""
    try:
        mnt_abs = os.path.realpath(mnt)
    except OSError:
        return
    try:
        names = os.listdir("/proc/self/fd")
    except OSError:
        return
    for n in names:
        try:
            fd = int(n)
        except ValueError:
            continue
        if fd < 3:
            continue
        try:
            tgt = os.readlink("/proc/self/fd/%d" % fd)
        except OSError:
            continue
        if not tgt.startswith("/"):
            continue
        try:
            real = os.path.realpath(tgt)
        except OSError:
            continue
        if real == mnt_abs or real.startswith(mnt_abs + os.sep):
            try:
                os.close(fd)
            except OSError:
                pass


# ==========================================================================
# Basic file I/O
# ==========================================================================
@test
def basic_write_read(d):
    p = os.path.join(d, "f")
    wr(p, b"hello efs")
    eq(rd(p), b"hello efs", "read back")


@test
def basic_empty_file(d):
    p = os.path.join(d, "empty")
    open(p, "wb").close()
    eq(os.path.getsize(p), 0, "empty size")
    eq(rd(p), b"", "empty read")


@test
def basic_append(d):
    p = os.path.join(d, "f")
    wr(p, b"aaa")
    with open(p, "ab") as f:
        f.write(b"bbb")
    eq(rd(p), b"aaabbb", "append")


@test
def basic_oappend_flag(d):
    p = os.path.join(d, "f")
    wr(p, b"aaa")
    fd = os.open(p, os.O_WRONLY | os.O_APPEND)
    os.lseek(fd, 0, os.SEEK_SET)          # seek ignored under O_APPEND
    os.write(fd, b"ZZZ")
    os.close(fd)
    eq(rd(p), b"aaaZZZ", "O_APPEND writes at end despite lseek")


@test
def basic_seek_read(d):
    p = os.path.join(d, "f")
    wr(p, b"0123456789")
    fd = os.open(p, os.O_RDONLY)
    os.lseek(fd, 4, os.SEEK_SET)
    eq(os.read(fd, 3), b"456", "seek+read")
    os.close(fd)


@test
def basic_overwrite_middle(d):
    p = os.path.join(d, "f")
    wr(p, b"0123456789")
    fd = os.open(p, os.O_RDWR)
    os.lseek(fd, 3, os.SEEK_SET)
    os.write(fd, b"XXX")
    os.close(fd)
    eq(rd(p), b"012XXX6789", "overwrite middle")


@test
def basic_read_past_eof(d):
    p = os.path.join(d, "f")
    wr(p, b"short")
    fd = os.open(p, os.O_RDONLY)
    os.lseek(fd, 100, os.SEEK_SET)
    eq(os.read(fd, 10), b"", "read past EOF returns empty")
    os.close(fd)


@test
def basic_terminal_cp_cat(d):
    # exercise real terminal commands
    wr(os.path.join(d, "src"), b"terminal-io")
    sh(["cp", "src", "dst"], cwd=d)
    eq(rd(os.path.join(d, "dst")), b"terminal-io", "cp content")
    r = sh("cat dst", cwd=d)
    eq(r.stdout, b"terminal-io", "cat output")
    r = sh("echo hello >> dst", cwd=d)
    eq(rd(os.path.join(d, "dst")), b"terminal-iohello\n", "echo append")


@test
def basic_dd_rw(d):
    # dd a seeded-random source (not /dev/zero) so zero-fill / repetitive-data
    # bugs can't hide; verify the copied content byte-for-byte.
    data = rand_bytes("basic_dd_rw", 32768)
    wr(os.path.join(d, "src"), data)
    sh("dd if=src of=f bs=4k count=8 2>/dev/null", cwd=d)
    eq(os.path.getsize(os.path.join(d, "f")), 32768, "dd size")
    eq(rd(os.path.join(d, "f")), data, "dd copied random content")


@test
def basic_o_trunc(d):
    p = os.path.join(d, "f")
    wr(p, b"0123456789")
    fd = os.open(p, os.O_WRONLY | os.O_TRUNC)
    os.write(fd, b"Z")
    os.close(fd)
    eq(rd(p), b"Z", "O_TRUNC then write")
    eq(os.path.getsize(p), 1, "O_TRUNC size")


@test
def basic_pread_pwrite(d):
    p = os.path.join(d, "f")
    wr(p, b"0123456789")
    fd = os.open(p, os.O_RDWR)
    try:
        eq(os.pread(fd, 3, 4), b"456", "pread")
        os.pwrite(fd, b"XXX", 2)
        eq(os.pread(fd, 10, 0), b"01XXX56789", "pwrite visible via pread")
        eq(os.lseek(fd, 0, os.SEEK_CUR), 0, "pwrite does not move offset")
    finally:
        os.close(fd)


@test
def basic_dup_write(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    os.write(fd, b"ab")
    fd2 = os.dup(fd)
    os.write(fd2, b"cd")
    os.close(fd)
    os.lseek(fd2, 0, os.SEEK_SET)
    eq(os.read(fd2, 4), b"abcd", "dup shares offset and data")
    os.close(fd2)


# efs default data chunk is 128 KiB; straddle + exact boundary.
_CHUNK = 128 * 1024


@test
def basic_chunk_boundary(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        os.write(fd, b"A" * _CHUNK)
        os.write(fd, b"B")
        os.lseek(fd, _CHUNK - 2, os.SEEK_SET)
        os.write(fd, b"XY")
        eq(os.pread(fd, 3, _CHUNK - 2), b"XYB", "straddle chunk boundary")
        eq(os.pread(fd, 1, _CHUNK), b"B", "first byte of next chunk")
    finally:
        os.close(fd)
    eq(os.path.getsize(p), _CHUNK + 1, "size after boundary write")


@test
def fsync_reopen_visible(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    os.write(fd, b"quorum")
    os.fsync(fd)
    os.close(fd)
    fd2 = os.open(p, os.O_RDONLY)
    eq(os.read(fd2, 6), b"quorum", "fsync then new fd sees data")
    os.close(fd2)


@test
def basic_ftruncate_vs_truncate(d):
    p = os.path.join(d, "f")
    wr(p, b"A" * 8000)
    fd = os.open(p, os.O_RDWR)
    os.ftruncate(fd, 2000)
    os.close(fd)
    eq(os.path.getsize(p), 2000, "ftruncate size")
    eq(rd(p), b"A" * 2000, "ftruncate content")
    os.truncate(p, 500)
    eq(os.path.getsize(p), 500, "path truncate size")


# W43: a truncate that spans > 32 chunks in a metadata lane must either
# happen completely or fail honestly (EIO/EBUSY) with the file left
# byte-identical — the old bug answered OK and silently kept the old size.
# With 64 lanes x 32 chunks x 128 KiB (EFS_CHUNK_SIZE) the threshold is
# 256 MiB, so the test file is 300 MiB, same as W43's stress gate. Both
# outcomes PASS here; only the lie FAILs (the ext4 baseline always takes
# the success path). @serial: two lane-spanning truncates in flight at once
# can leave the refused file's reads transiently failing (EIO, ~1 s while
# the lane settles) — observed on a fast loopback cluster; _verify_big_
# unchanged retries through that window.
TRUNC_BIG_TOTAL = 300 * 1024 * 1024
TRUNC_BIG_KEEP = 10 * 1024 * 1024
_TRUNC_BLK = bytes(bytearray(range(256))) * 4096   # 1 MiB non-zero pattern


def _write_big(path, total):
    with open(path, "wb") as f:
        for _ in range(total // len(_TRUNC_BLK)):
            f.write(_TRUNC_BLK)
        f.flush()
        os.fsync(f.fileno())


def _verify_big_unchanged(path, total):
    """Refused truncate: the file must be byte-identical (a partial apply
    behind an EIO is still a lie). A big truncate refused while ANOTHER big
    truncate is in flight can leave the file's reads failing with EIO for
    ~1 s while the lane settles — retry through that window, but never
    tolerate a changed size or byte."""
    deadline = time.time() + 10
    while True:
        try:
            eq(os.path.getsize(path), total, "size after refused truncate")
            with open(path, "rb") as f:
                while True:
                    b = f.read(len(_TRUNC_BLK))
                    if not b:
                        break
                    eq(b, _TRUNC_BLK[:len(b)], "content after refused truncate")
            return
        except OSError as e:
            if e.errno != errno.EIO or time.time() >= deadline:
                raise
            time.sleep(0.3)


@test
@budget(180)
@serial
def truncate_big_ftruncate_honest(d):
    p = os.path.join(d, "big")
    _write_big(p, TRUNC_BIG_TOTAL)
    with open(p, "rb") as f:
        head = f.read(TRUNC_BIG_KEEP)
    try:
        os.truncate(p, TRUNC_BIG_KEEP)
    except OSError as e:
        if e.errno not in (errno.EIO, errno.EBUSY):
            raise
        _verify_big_unchanged(p, TRUNC_BIG_TOTAL)
    else:
        eq(os.path.getsize(p), TRUNC_BIG_KEEP, "size after truncate")
        eq(rd(p), head, "prefix content after truncate")


@test
@budget(180)
@serial
def truncate_big_o_trunc_honest(d):
    p = os.path.join(d, "big")
    _write_big(p, TRUNC_BIG_TOTAL)
    try:
        fd = os.open(p, os.O_WRONLY | os.O_TRUNC)
    except OSError as e:
        if e.errno not in (errno.EIO, errno.EBUSY):
            raise
        _verify_big_unchanged(p, TRUNC_BIG_TOTAL)
        return
    new = _TRUNC_BLK * (TRUNC_BIG_KEEP // len(_TRUNC_BLK))
    with os.fdopen(fd, "wb") as f:
        f.write(new)
        f.flush()
        os.fsync(f.fileno())
    eq(os.path.getsize(p), TRUNC_BIG_KEEP, "size after O_TRUNC rewrite")
    eq(rd(p), new, "content after O_TRUNC rewrite")


@test
def basic_seek_end_then_write(d):
    p = os.path.join(d, "f")
    wr(p, b"abc")
    fd = os.open(p, os.O_RDWR)
    os.lseek(fd, 0, os.SEEK_END)
    os.write(fd, b"def")
    os.close(fd)
    eq(rd(p), b"abcdef", "SEEK_END write")


@test
def basic_seek_cur_past_eof_write(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    os.write(fd, b"XY")
    os.lseek(fd, 8, os.SEEK_CUR)   # offset 10
    os.write(fd, b"Z")
    os.close(fd)
    eq(os.path.getsize(p), 11, "implicit hole size")
    eq(rd(p), b"XY" + b"\x00" * 8 + b"Z", "hole is zeros")


@test
def basic_rdwr_no_reopen(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    os.write(fd, b"ping")
    os.lseek(fd, 0, os.SEEK_SET)
    eq(os.read(fd, 4), b"ping", "read same fd after write")
    os.close(fd)


# ==========================================================================
# Truncate + sparse files
# ==========================================================================
@test
def trunc_shrink(d):
    p = os.path.join(d, "f")
    wr(p, b"A" * 10000)
    os.truncate(p, 4000)
    eq(os.path.getsize(p), 4000, "shrink size")
    eq(rd(p), b"A" * 4000, "shrink content")


@test
def trunc_grow_sparse(d):
    p = os.path.join(d, "f")
    wr(p, b"A" * 100)
    os.truncate(p, 100000)
    eq(os.path.getsize(p), 100000, "grow size")
    fd = os.open(p, os.O_RDONLY)
    os.lseek(fd, 50000, os.SEEK_SET)
    eq(os.read(fd, 10), b"\x00" * 10, "hole reads as zeros")
    os.close(fd)


@test
def sparse_write_huge_offset(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    os.lseek(fd, 1 << 20, os.SEEK_SET)    # 1 MiB offset
    os.write(fd, b"END")
    os.close(fd)
    eq(os.path.getsize(p), (1 << 20) + 3, "sparse size")
    fd = os.open(p, os.O_RDONLY)
    os.lseek(fd, 100, os.SEEK_SET)
    eq(os.read(fd, 4), b"\x00" * 4, "sparse hole zeros")
    os.lseek(fd, 1 << 20, os.SEEK_SET)
    eq(os.read(fd, 3), b"END", "sparse tail")
    os.close(fd)


@test
def trunc_to_zero(d):
    p = os.path.join(d, "f")
    wr(p, b"data")
    os.truncate(p, 0)
    eq(os.path.getsize(p), 0, "truncate to 0")


@test
def sparse_st_blocks(d):
    """A 1 MiB hole should not count as fully allocated in st_blocks."""
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    os.lseek(fd, 1 << 20, os.SEEK_SET)
    os.write(fd, b"END")
    os.close(fd)
    st = os.stat(p)
    eq(st.st_size, (1 << 20) + 3, "sparse size")
    allocated = st.st_blocks * 512
    if allocated >= st.st_size:
        raise Fail("st_blocks=%d implies %d allocated bytes for a 1MiB hole "
                   "(st_size=%d); holes look fully allocated" %
                   (st.st_blocks, allocated, st.st_size))


# ==========================================================================
# Directory operations
# ==========================================================================
@test
def dir_mkdir_rmdir(d):
    p = os.path.join(d, "sub")
    os.mkdir(p)
    assert os.path.isdir(p)
    os.rmdir(p)
    assert not os.path.exists(p)


@test
def dir_mkdir_p_terminal(d):
    sh("mkdir -p a/b/c/d/e", cwd=d)
    assert os.path.isdir(os.path.join(d, "a/b/c/d/e"))


@test
def dir_readdir_listing(d):
    names = ["f%d" % i for i in range(50)]
    for n in names:
        wr(os.path.join(d, n), b"x")
    listed = set(os.listdir(d))
    eq(listed, set(names), "readdir returns all entries")
    # ls terminal command agrees
    r = sh("ls", cwd=d)
    eq(set(r.stdout.decode().split()), set(names), "ls listing")


@test
def dir_rename_file(d):
    wr(os.path.join(d, "a"), b"data")
    os.rename(os.path.join(d, "a"), os.path.join(d, "b"))
    assert not os.path.exists(os.path.join(d, "a"))
    eq(rd(os.path.join(d, "b")), b"data", "rename preserves content")


@test
def dir_rename_over_existing(d):
    wr(os.path.join(d, "a"), b"new")
    wr(os.path.join(d, "b"), b"old")
    os.rename(os.path.join(d, "a"), os.path.join(d, "b"))  # replace
    eq(rd(os.path.join(d, "b")), b"new", "rename replaces target")


@test
def dir_rename_dir_with_contents(d):
    os.makedirs(os.path.join(d, "src/sub"))
    wr(os.path.join(d, "src/sub/f"), b"nested")
    os.rename(os.path.join(d, "src"), os.path.join(d, "dst"))
    eq(rd(os.path.join(d, "dst/sub/f")), b"nested", "dir rename keeps contents")


@test
def dir_move_into_subdir(d):
    os.makedirs(os.path.join(d, "a"))
    os.makedirs(os.path.join(d, "b"))
    wr(os.path.join(d, "a/f"), b"x")
    os.rename(os.path.join(d, "a/f"), os.path.join(d, "b/f"))
    eq(rd(os.path.join(d, "b/f")), b"x", "move across dirs")


@test
def dir_rename_file_over_dir(d):
    wr(os.path.join(d, "f"), b"x")
    os.mkdir(os.path.join(d, "sub"))
    expect_err((errno.EISDIR, errno.ENOTDIR), os.rename,
               os.path.join(d, "f"), os.path.join(d, "sub"))


@test
def dir_rename_dir_over_file(d):
    os.mkdir(os.path.join(d, "sub"))
    wr(os.path.join(d, "f"), b"x")
    expect_err((errno.ENOTDIR, errno.EISDIR), os.rename,
               os.path.join(d, "sub"), os.path.join(d, "f"))


@test
def dir_rename_dir_over_nonempty(d):
    os.makedirs(os.path.join(d, "a"))
    os.makedirs(os.path.join(d, "b"))
    wr(os.path.join(d, "b/f"), b"x")
    expect_err((errno.ENOTEMPTY, errno.EEXIST), os.rename,
               os.path.join(d, "a"), os.path.join(d, "b"))


@test
def dir_rename_noreplace(d):
    wr(os.path.join(d, "a"), b"new")
    wr(os.path.join(d, "b"), b"old")
    flags = getattr(os, "RENAME_NOREPLACE", 1)
    if hasattr(os, "renameat2"):
        expect_err(errno.EEXIST, os.renameat2,
                   os.path.join(d, "a"), os.path.join(d, "b"), flags=flags)
    else:
        libc = __import__("ctypes").CDLL("libc.so.6", use_errno=True)
        AT_FDCWD = -100
        rc = libc.renameat2(AT_FDCWD, os.path.join(d, "a").encode(),
                            AT_FDCWD, os.path.join(d, "b").encode(), flags)
        if rc == 0:
            raise Fail("RENAME_NOREPLACE succeeded over existing file")
        err = __import__("ctypes").get_errno()
        if err != errno.EEXIST:
            raise Fail("RENAME_NOREPLACE errno %s, want EEXIST" % err)
    eq(rd(os.path.join(d, "b")), b"old", "target unchanged")
    eq(rd(os.path.join(d, "a")), b"new", "source still there")


@test
def dir_rename_symlink(d):
    wr(os.path.join(d, "t"), b"data")
    os.symlink("t", os.path.join(d, "sl"))
    os.rename(os.path.join(d, "sl"), os.path.join(d, "sl2"))
    eq(os.readlink(os.path.join(d, "sl2")), "t", "rename moves the symlink")
    assert not os.path.exists(os.path.join(d, "sl"))
    eq(rd(os.path.join(d, "t")), b"data", "target intact")


# --------------------------------------------------------------------------
# Mount-root operations (@root: the test gets the mount root, not a scratch
# dir). The export root is a sharded directory — its dentries live on
# different metadata shards than a nested dir's — and every test above runs
# in a scratch subdir, so this path otherwise goes unexercised. W56: a
# rename whose parent was the export root left the OLD name resolvable on
# the renaming client until remount, while every other view was correct.
# --------------------------------------------------------------------------

def _root_pair(mnt, tag):
    """Unique (pid-tagged) entry names in the mount root: the mount may be
    shared with sibling suite runs (POSIX_PER_HOST)."""
    a = os.path.join(mnt, "posixroot-%s-%d-a" % (tag, os.getpid()))
    b = os.path.join(mnt, "posixroot-%s-%d-b" % (tag, os.getpid()))
    return a, b


def _root_cleanup(*paths):
    for p in paths:
        try:
            if os.path.isdir(p) and not os.path.islink(p):
                shutil.rmtree(p, ignore_errors=True)
            else:
                os.unlink(p)
        except OSError:
            pass  # already gone, or a W56 ghost that only resolves locally


@test
@root
def root_rename_dir_old_name_gone(mnt):
    a, b = _root_pair(mnt, "rendir")
    try:
        os.mkdir(a)
        os.rename(a, b)
        expect_err(errno.ENOENT, os.stat, a)   # the W56 ghost
        eq(os.path.isdir(b), True, "renamed dir resolves at the new name")
        # ...while the parent readdir was correct all along — pin both views
        listed = os.listdir(mnt)
        if os.path.basename(a) in listed:
            raise Fail("old name still in root readdir after rename")
        if os.path.basename(b) not in listed:
            raise Fail("new name missing from root readdir after rename")
    finally:
        _root_cleanup(a, b)


@test
@root
def root_rename_file_old_name_gone(mnt):
    a, b = _root_pair(mnt, "renfile")
    try:
        wr(a, b"data")
        os.rename(a, b)
        expect_err(errno.ENOENT, os.stat, a)
        eq(rd(b), b"data", "content at the new name")
    finally:
        _root_cleanup(a, b)


@test
@root
def root_rename_from_subdir(mnt):
    """nested -> root: the clean-shard direction; the new root dentry must
    resolve immediately on the renaming client."""
    a, b = _root_pair(mnt, "renup")
    try:
        os.mkdir(a)
        f = os.path.join(a, "f")
        wr(f, b"x")
        os.rename(f, b)
        expect_err(errno.ENOENT, os.stat, f)
        eq(rd(b), b"x", "content at the root name")
    finally:
        _root_cleanup(a, b)


@test
@root
def root_rename_into_subdir(mnt):
    """root -> nested: the source parent is the sharded root — the same W56
    ghost path as a root -> root rename."""
    a, b = _root_pair(mnt, "rendown")
    try:
        wr(a, b"x")
        os.mkdir(b)
        dst = os.path.join(b, "f")
        os.rename(a, dst)
        expect_err(errno.ENOENT, os.stat, a)
        eq(rd(dst), b"x", "content at the nested name")
    finally:
        _root_cleanup(a, b)


@test
@root
def root_create_unlink_immediate(mnt):
    a, _b = _root_pair(mnt, "cre")
    try:
        wr(a, b"x")
        eq(rd(a), b"x", "created at the root")
        os.unlink(a)
        expect_err(errno.ENOENT, os.stat, a)
    finally:
        _root_cleanup(a)


@test
@root
def root_mkdir_rmdir_immediate(mnt):
    a, _b = _root_pair(mnt, "mkd")
    try:
        os.mkdir(a)
        eq(os.path.isdir(a), True, "mkdir at the root")
        os.rmdir(a)
        expect_err(errno.ENOENT, os.stat, a)
    finally:
        _root_cleanup(a)


@test
def dir_deep_nesting(d):
    depth = 60
    parts = ["d%02d" % i for i in range(depth)]
    deep = os.path.join(d, *parts)
    os.makedirs(deep)
    wr(os.path.join(deep, "leaf"), b"deep")
    eq(rd(os.path.join(deep, "leaf")), b"deep", "deep file")
    # walk back out
    shutil.rmtree(os.path.join(d, parts[0]))
    assert not os.path.exists(os.path.join(d, parts[0]))


@test
@budget(75)
def dir_many_files(d):
    n = 300
    for i in range(n):
        wr(os.path.join(d, "f%04d" % i), b"x")
    eq(len(os.listdir(d)), n, "many files listed")
    for i in range(n):
        os.unlink(os.path.join(d, "f%04d" % i))
    eq(len(os.listdir(d)), 0, "many files removed")


# ==========================================================================
# Directory timestamps: entry changes bump the parent's mtime+ctime (§7.4).
# The bumps observed through a long-lived mount do not always advance
# sub-second (creation rows were seen at whole-second granularity), so the
# old 0.06 s re-stat wait could not see a legitimate bump and failed
# spuriously on fast clusters. Sleep a full second instead: any client/
# server clock offset still crosses a second boundary within 1 s, and the
# wait also clears the 50 ms lookup-memo window (0j).
# ==========================================================================
def _dir_times(p):
    st = os.stat(p)
    return st.st_mtime_ns, st.st_ctime_ns


def _wait_dir_tick():
    time.sleep(1.06)


def _expect_dir_bump(p, before, what):
    m, c = _dir_times(p)
    if m <= before[0]:
        raise Fail("%s: directory mtime did not advance (%d -> %d)" %
                   (what, before[0], m))
    if c <= before[1]:
        raise Fail("%s: directory ctime did not advance (%d -> %d)" %
                   (what, before[1], c))


@test
def dir_times_create(d):
    before = _dir_times(d)
    _wait_dir_tick()
    wr(os.path.join(d, "f"), b"x")
    _expect_dir_bump(d, before, "create")


@test
def dir_times_unlink(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    before = _dir_times(d)
    _wait_dir_tick()
    os.unlink(p)
    _expect_dir_bump(d, before, "unlink")


@test
def dir_times_mkdir_rmdir(d):
    sub = os.path.join(d, "sub")
    before = _dir_times(d)
    _wait_dir_tick()
    os.mkdir(sub)
    _expect_dir_bump(d, before, "mkdir")
    before = _dir_times(d)
    _wait_dir_tick()
    os.rmdir(sub)
    _expect_dir_bump(d, before, "rmdir")


@test
def dir_times_rename(d):
    a = os.path.join(d, "a")
    wr(a, b"x")
    before = _dir_times(d)
    _wait_dir_tick()
    os.rename(a, os.path.join(d, "b"))
    _expect_dir_bump(d, before, "rename same dir")
    # Cross-directory rename bumps BOTH parents. The 0.1 s post-rename wait
    # lands the stats outside the 50 ms rename-reply memo, so they measure
    # the client's settled view. Known bug: on the RENAMING client the dst
    # parent's attrs are never invalidated by the rename — it keeps serving
    # the pre-rename times indefinitely (>= 25 s, readdir does not refresh)
    # while other mounts see the bump immediately. Parent->child layout: the
    # src parent survives (it is an ancestor of the dst path and gets
    # refreshed along the way).
    sub = os.path.join(d, "sub")
    os.mkdir(sub)
    before_d = _dir_times(d)
    before_sub = _dir_times(sub)
    _wait_dir_tick()
    os.rename(os.path.join(d, "b"), os.path.join(sub, "b"))
    time.sleep(0.1)
    _expect_dir_bump(d, before_d, "rename src dir")
    _expect_dir_bump(sub, before_sub, "rename dst dir")
    # Sibling layout: neither parent is an ancestor of the dst path, so the
    # renaming client goes stale on BOTH.
    sa = os.path.join(d, "sa")
    sb = os.path.join(d, "sb")
    os.mkdir(sa)
    os.mkdir(sb)
    wr(os.path.join(sa, "f"), b"x")
    before_sa = _dir_times(sa)
    before_sb = _dir_times(sb)
    _wait_dir_tick()
    os.rename(os.path.join(sa, "f"), os.path.join(sb, "f"))
    time.sleep(0.1)
    _expect_dir_bump(sa, before_sa, "rename sibling src dir")
    _expect_dir_bump(sb, before_sb, "rename sibling dst dir")


@test
def dir_times_link(d):
    wr(os.path.join(d, "f"), b"x")
    sub = os.path.join(d, "sub")
    os.mkdir(sub)
    before = _dir_times(sub)
    _wait_dir_tick()
    os.link(os.path.join(d, "f"), os.path.join(sub, "g"))
    _expect_dir_bump(sub, before, "link into dir")


@test
def dir_times_write_no_bump(d):
    """File content writes do NOT touch the containing directory (POSIX;
    §7.4 puts write times in the file's lanes, not on the parent)."""
    p = os.path.join(d, "f")
    wr(p, b"x")
    before = _dir_times(d)
    time.sleep(0.06)
    wr(p, b"yz")
    eq(_dir_times(d), before, "write leaves dir mtime/ctime alone")


@test
def dir_dot_entries(d):
    os.mkdir(os.path.join(d, "sub"))
    listed = set(os.listdir(os.path.join(d, "sub")))
    eq(listed, set(), "empty dir lists nothing (listdir hides . ..)")
    r = sh("ls -a sub", cwd=d)
    out = set(r.stdout.decode().split())
    eq(out, {".", ".."}, "ls -a shows only . and .. in empty dir")


@test
def dir_hidden_files(d):
    wr(os.path.join(d, ".hidden"), b"secret")
    wr(os.path.join(d, "visible"), b"ok")
    wr(os.path.join(d, ".dot.mid"), b"x")
    listed = set(os.listdir(d))
    eq(listed, {".hidden", "visible", ".dot.mid"}, "listdir includes hidden")
    r = sh("ls", cwd=d)
    eq(set(r.stdout.decode().split()), {"visible"}, "ls hides dotfiles")
    r = sh("ls -a", cwd=d)
    eq(set(r.stdout.decode().split()), {".", "..", ".hidden", "visible", ".dot.mid"},
       "ls -a shows hidden")
    eq(rd(os.path.join(d, ".hidden")), b"secret", "hidden content")
    os.unlink(os.path.join(d, ".hidden"))
    assert not os.path.exists(os.path.join(d, ".hidden"))


@test
@serial
def dir_mkdir_umask(d):
    old = os.umask(0o022)
    try:
        os.mkdir(os.path.join(d, "sub"), 0o777)
        eq(statmod.S_IMODE(os.stat(os.path.join(d, "sub")).st_mode), 0o755,
           "mkdir umask 022 -> 755")
    finally:
        os.umask(old)


@test
def dir_chmod_and_create_denied(d):
    sub = os.path.join(d, "sub")
    os.mkdir(sub)
    os.chmod(sub, 0o500)
    eq(statmod.S_IMODE(os.stat(sub).st_mode), 0o500, "dir chmod")
    if os.geteuid() != 0:
        expect_err(errno.EACCES, wr, os.path.join(sub, "f"), b"x")
    os.chmod(sub, 0o755)


@test
def dir_nlink(d):
    sub = os.path.join(d, "sub")
    os.mkdir(sub)
    n0 = os.stat(sub).st_nlink
    os.mkdir(os.path.join(sub, "a"))
    eq(os.stat(sub).st_nlink, n0 + 1, "mkdir child bumps dir nlink")
    os.rmdir(os.path.join(sub, "a"))
    eq(os.stat(sub).st_nlink, n0, "rmdir child restores nlink")


@test
def dir_readdir_while_unlink(d):
    for i in range(40):
        wr(os.path.join(d, "f%02d" % i), b"x")
    fd = os.open(d, os.O_RDONLY)
    try:
        it = os.scandir(d)
        seen = 0
        for i, ent in enumerate(it):
            seen += 1
            if i == 5:
                try:
                    os.unlink(os.path.join(d, "f00"))
                except OSError:
                    pass
        it.close()
        if seen < 2:
            raise Fail("scandir died after mutate, saw %d" % seen)
    finally:
        os.close(fd)


# ==========================================================================
# Links: symlinks + hardlinks
# ==========================================================================
@test
def symlink_create_readlink(d):
    wr(os.path.join(d, "target"), b"data")
    os.symlink("target", os.path.join(d, "sl"))
    eq(os.readlink(os.path.join(d, "sl")), "target", "readlink")
    assert statmod.S_ISLNK(os.lstat(os.path.join(d, "sl")).st_mode)
    eq(rd(os.path.join(d, "sl")), b"data", "read through symlink")


@test
def symlink_to_dir(d):
    os.mkdir(os.path.join(d, "realdir"))
    wr(os.path.join(d, "realdir/f"), b"x")
    os.symlink("realdir", os.path.join(d, "dirlink"))
    eq(rd(os.path.join(d, "dirlink/f")), b"x", "read through dir symlink")


@test
def symlink_dangling(d):
    os.symlink("nonexistent", os.path.join(d, "dangle"))
    eq(os.readlink(os.path.join(d, "dangle")), "nonexistent", "dangling readlink")
    # stat follows -> ENOENT; lstat works
    expect_err(errno.ENOENT, os.stat, os.path.join(d, "dangle"))
    assert statmod.S_ISLNK(os.lstat(os.path.join(d, "dangle")).st_mode)


@test
def symlink_chain(d):
    wr(os.path.join(d, "c"), b"end")
    os.symlink("c", os.path.join(d, "b"))
    os.symlink("b", os.path.join(d, "a"))
    eq(rd(os.path.join(d, "a")), b"end", "symlink chain resolves")


@test
def symlink_absolute(d):
    wr(os.path.join(d, "t"), b"abs")
    os.symlink(os.path.join(d, "t"), os.path.join(d, "absl"))
    eq(rd(os.path.join(d, "absl")), b"abs", "absolute symlink")


@test
def hardlink_basic(d):
    wr(os.path.join(d, "orig"), b"shared")
    os.link(os.path.join(d, "orig"), os.path.join(d, "hl"))
    eq(rd(os.path.join(d, "hl")), b"shared", "hardlink reads same data")
    eq(os.stat(os.path.join(d, "orig")).st_nlink, 2, "nlink=2")
    eq(os.stat(os.path.join(d, "orig")).st_ino,
       os.stat(os.path.join(d, "hl")).st_ino, "same inode")


@test
def hardlink_shared_data(d):
    wr(os.path.join(d, "orig"), b"aaa")
    os.link(os.path.join(d, "orig"), os.path.join(d, "hl"))
    with open(os.path.join(d, "hl"), "ab") as f:
        f.write(b"bbb")
    eq(rd(os.path.join(d, "orig")), b"aaabbb", "write via link seen via orig")


@test
def hardlink_unlink_one(d):
    wr(os.path.join(d, "orig"), b"data")
    os.link(os.path.join(d, "orig"), os.path.join(d, "hl"))
    os.unlink(os.path.join(d, "orig"))
    eq(rd(os.path.join(d, "hl")), b"data", "data survives unlink of one link")
    eq(os.stat(os.path.join(d, "hl")).st_nlink, 1, "nlink back to 1")


@test
def hardlink_terminal_ln(d):
    wr(os.path.join(d, "orig"), b"x")
    sh(["ln", "orig", "hl"], cwd=d)
    eq(os.stat(os.path.join(d, "orig")).st_nlink, 2, "ln bumps nlink")


@test
def hardlink_rename_one_name(d):
    wr(os.path.join(d, "orig"), b"shared")
    os.link(os.path.join(d, "orig"), os.path.join(d, "hl"))
    os.rename(os.path.join(d, "orig"), os.path.join(d, "moved"))
    eq(rd(os.path.join(d, "hl")), b"shared", "other name after rename")
    eq(os.stat(os.path.join(d, "hl")).st_ino,
       os.stat(os.path.join(d, "moved")).st_ino, "same ino after rename")


@test
def symlink_unlink_keeps_target(d):
    wr(os.path.join(d, "t"), b"keep")
    os.symlink("t", os.path.join(d, "sl"))
    os.unlink(os.path.join(d, "sl"))
    eq(rd(os.path.join(d, "t")), b"keep", "unlink symlink leaves target")


@test
def symlink_relative_after_parent_rename(d):
    os.makedirs(os.path.join(d, "a"))
    wr(os.path.join(d, "t"), b"rel")
    os.symlink("../t", os.path.join(d, "a/sl"))
    eq(rd(os.path.join(d, "a/sl")), b"rel", "relative symlink before rename")
    os.rename(os.path.join(d, "a"), os.path.join(d, "b"))
    eq(os.readlink(os.path.join(d, "b/sl")), "../t", "link text unchanged")
    eq(rd(os.path.join(d, "b/sl")), b"rel", "relative still resolves")


@test
def symlink_chmod_follows(d):
    p = os.path.join(d, "t")
    wr(p, b"x")
    os.symlink("t", os.path.join(d, "sl"))
    os.chmod(os.path.join(d, "sl"), 0o600)
    eq(statmod.S_IMODE(os.stat(p).st_mode), 0o600, "chmod via symlink follows")
    assert statmod.S_ISLNK(os.lstat(os.path.join(d, "sl")).st_mode)


# ==========================================================================
# Attributes: chmod/chown/utimens/stat/access
# ==========================================================================
@test
def attr_chmod(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    for mode in (0o000, 0o444, 0o600, 0o755, 0o777):
        os.chmod(p, mode)
        eq(statmod.S_IMODE(os.stat(p).st_mode), mode, "chmod %o" % mode)


@test
def attr_chown_self(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.chown(p, os.getuid(), os.getgid())   # no-op but exercises the path
    eq(os.stat(p).st_uid, os.getuid(), "uid")


@test
def attr_utimens(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.utime(p, (1000000000, 1234567890))
    eq(int(os.stat(p).st_mtime), 1234567890, "mtime set")
    eq(int(os.stat(p).st_atime), 1000000000, "atime set")


@test
def attr_utimens_ns(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.utime(p, ns=(1000000000123456789, 1234567890987654321))
    st = os.stat(p)
    eq(st.st_mtime_ns, 1234567890987654321, "mtime ns precision")


@test
def attr_touch_terminal(d):
    wr(os.path.join(d, "f"), b"x")
    sh("touch -d '2001-09-09 01:46:40 UTC' f", cwd=d)
    eq(int(os.stat(os.path.join(d, "f")).st_mtime), 1000000000, "touch -d")


@test
def attr_stat_fields(d):
    p = os.path.join(d, "f")
    wr(p, b"12345")
    st = os.stat(p)
    eq(st.st_size, 5, "size")
    assert statmod.S_ISREG(st.st_mode), "is regular file"
    eq(st.st_nlink, 1, "nlink")


@test
@serial
def attr_umask_respected(d):
    old = os.umask(0o022)
    try:
        p = os.path.join(d, "f")
        fd = os.open(p, os.O_CREAT | os.O_WRONLY, 0o666)
        os.close(fd)
        eq(statmod.S_IMODE(os.stat(p).st_mode), 0o644, "umask 022 -> 644")
    finally:
        os.umask(old)


@test
def attr_access(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.chmod(p, 0o444)
    assert os.access(p, os.R_OK), "readable"
    if os.geteuid() != 0:   # root bypasses permission checks
        assert not os.access(p, os.W_OK), "not writable"


@test
def attr_ctime_after_chmod(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    c0 = os.stat(p).st_ctime_ns
    time.sleep(0.05)
    os.chmod(p, 0o600)
    c1 = os.stat(p).st_ctime_ns
    if c1 < c0:
        raise Fail("ctime went backwards after chmod")
    if c1 == c0:
        raise Fail("ctime unchanged after chmod")


# ==========================================================================
# Permissions (unprivileged user; root bypasses — those tests no-op)
# ==========================================================================
def _root():
    return os.geteuid() == 0


def _skip_root():
    """Root bypasses DAC. That is a skipped check, not a pass."""
    if _root():
        raise Fail("skipped: root bypasses permission checks", soft=True)


def _write_script(path):
    with open(path, "w") as f:
        f.write("#!%s\nimport sys\nsys.exit(0)\n" % sys.executable)


@test
def perm_file_000_denied(d):
    _skip_root()
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.chmod(p, 0o000)
    expect_err(errno.EACCES, open, p, "rb")
    expect_err(errno.EACCES, open, p, "wb")
    expect_err(errno.EACCES, open, p, "ab")
    assert not os.access(p, os.R_OK)
    assert not os.access(p, os.W_OK)
    assert not os.access(p, os.X_OK)


@test
def perm_file_write_only(d):
    _skip_root()
    p = os.path.join(d, "f")
    wr(p, b"old")
    os.chmod(p, 0o200)
    with open(p, "wb") as f:
        f.write(b"new")
    expect_err(errno.EACCES, open, p, "rb")
    assert os.access(p, os.W_OK)
    assert not os.access(p, os.R_OK)


@test
def perm_file_read_only_ops(d):
    _skip_root()
    p = os.path.join(d, "f")
    wr(p, b"keep")
    os.chmod(p, 0o400)
    eq(rd(p), b"keep", "read still works")
    expect_err(errno.EACCES, open, p, "wb")
    expect_err(errno.EACCES, open, p, "ab")
    expect_err(errno.EACCES, os.open, p, os.O_RDWR)
    expect_err(errno.EACCES, os.open, p, os.O_WRONLY | os.O_TRUNC)
    expect_err(errno.EACCES, os.truncate, p, 1)
    eq(rd(p), b"keep", "denied writes did not mutate")


@test
def perm_owner_bits_not_other(d):
    _skip_root()
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.chmod(p, 0o007)   # --- --- rwx : owner has nothing
    expect_err(errno.EACCES, open, p, "rb")
    expect_err(errno.EACCES, open, p, "wb")
    assert not os.access(p, os.R_OK | os.W_OK | os.X_OK)


@test
def perm_owner_bits_not_group(d):
    _skip_root()
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.chmod(p, 0o070)
    expect_err(errno.EACCES, open, p, "rb")
    assert not os.access(p, os.R_OK)


@test
def perm_create_mode_000_reopen(d):
    _skip_root()
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_WRONLY, 0o000)
    os.write(fd, b"x")
    os.close(fd)
    eq(statmod.S_IMODE(os.stat(p).st_mode), 0o000, "create mode 000")
    expect_err(errno.EACCES, open, p, "rb")
    expect_err(errno.EACCES, open, p, "wb")


@test
def perm_open_fd_survives_chmod(d):
    _skip_root()
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    os.write(fd, b"abcd")
    os.chmod(p, 0o000)
    expect_err(errno.EACCES, open, p, "rb")   # new open denied
    os.lseek(fd, 0, os.SEEK_SET)
    eq(os.read(fd, 4), b"abcd", "existing fd still readable")
    os.lseek(fd, 0, os.SEEK_SET)
    os.write(fd, b"efgh")
    os.fsync(fd)
    os.close(fd)
    os.chmod(p, 0o644)
    eq(rd(p), b"efgh", "existing fd still writable")


@test
def perm_owner_chmod_restore(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.chmod(p, 0o000)
    os.chmod(p, 0o644)   # owner may chmod regardless of mode
    eq(rd(p), b"x", "readable after restore")


@test
def perm_owner_utime_readonly(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.chmod(p, 0o444)
    os.utime(p, (1000000000, 1000000000))   # owner may utime
    eq(int(os.stat(p).st_mtime), 1000000000, "owner utime on 0444")


@test
def perm_fchmod(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        os.fchmod(fd, 0o600)
    finally:
        os.close(fd)
    eq(statmod.S_IMODE(os.stat(p).st_mode), 0o600, "fchmod")


@test
def perm_ftruncate_after_chmod(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    os.write(fd, b"ABCDEFGH")
    os.chmod(p, 0o444)
    try:
        os.ftruncate(fd, 3)   # already-open fd, not a new open
    finally:
        os.close(fd)
    os.chmod(p, 0o644)
    eq(rd(p), b"ABC", "ftruncate via fd after 0444")


@test
def perm_access_rwx(d):
    _skip_root()
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.chmod(p, 0o644)
    assert os.access(p, os.R_OK), "644 R"
    assert os.access(p, os.W_OK), "644 W"
    if not _root():
        assert not os.access(p, os.X_OK), "644 not X"
    os.chmod(p, 0o755)
    assert os.access(p, os.R_OK | os.W_OK | os.X_OK), "755 RWX"
    os.chmod(p, 0o111)
    if not _root():
        assert os.access(p, os.X_OK), "111 X"
        assert not os.access(p, os.R_OK), "111 not R"
        assert not os.access(p, os.W_OK), "111 not W"


@test
def perm_exec_denied_no_x(d):
    _skip_root()
    p = os.path.join(d, "s")
    _write_script(p)
    os.chmod(p, 0o644)
    try:
        subprocess.run([p], timeout=8, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    except OSError as e:
        if e.errno == errno.EACCES:
            return
        raise Fail("exec 644: %s" % e)
    raise Fail("exec of mode 644 succeeded")


@test
def perm_exec_allowed_with_x(d):
    p = os.path.join(d, "s")
    _write_script(p)
    os.chmod(p, 0o755)
    r = subprocess.run([p], timeout=8, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    if r.returncode != 0:
        raise Fail("exec 755 rc=%d" % r.returncode)


@test
def perm_suid_sgid_sticky_stored(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.chmod(p, 0o4755)
    eq(statmod.S_IMODE(os.stat(p).st_mode), 0o4755, "suid")
    os.chmod(p, 0o2755)
    eq(statmod.S_IMODE(os.stat(p).st_mode), 0o2755, "sgid")
    os.chmod(p, 0o1755)
    eq(statmod.S_IMODE(os.stat(p).st_mode), 0o1755, "sticky file")
    sub = os.path.join(d, "sub")
    os.mkdir(sub)
    os.chmod(sub, 0o1755)
    eq(statmod.S_IMODE(os.stat(sub).st_mode), 0o1755, "sticky dir")


@test
def perm_sticky_owner_can_unlink(d):
    sub = os.path.join(d, "sub")
    os.mkdir(sub)
    os.chmod(sub, 0o1777)
    wr(os.path.join(sub, "f"), b"x")
    os.unlink(os.path.join(sub, "f"))   # owner always may; no 2nd uid here
    assert not os.path.exists(os.path.join(sub, "f"))


@test
def perm_chown_other_uid_denied(d):
    _skip_root()
    p = os.path.join(d, "f")
    wr(p, b"x")
    expect_err((errno.EPERM, errno.EACCES), os.chown, p, os.getuid() + 1, -1)
    eq(os.stat(p).st_uid, os.getuid(), "uid unchanged")


@test
def perm_chown_other_gid_denied(d):
    _skip_root()
    p = os.path.join(d, "f")
    wr(p, b"x")
    mine = set(os.getgroups())
    mine.add(os.getgid())
    target = None
    for g in (65534, 99, 1, 65535, 0):
        if g not in mine:
            target = g
            break
    if target is None:
        raise Fail("no foreign gid to test", soft=True)
    expect_err((errno.EPERM, errno.EACCES), os.chown, p, -1, target)
    eq(os.stat(p).st_gid, os.getgid(), "gid unchanged")


@test
def perm_dir_readonly_mutate(d):
    _skip_root()
    sub = os.path.join(d, "sub")
    os.mkdir(sub)
    wr(os.path.join(sub, "a"), b"x")
    wr(os.path.join(sub, "b"), b"y")
    os.chmod(sub, 0o555)
    try:
        expect_err(errno.EACCES, wr, os.path.join(sub, "c"), b"z")
        expect_err(errno.EACCES, os.mkdir, os.path.join(sub, "n"))
        expect_err(errno.EACCES, os.unlink, os.path.join(sub, "a"))
        expect_err(errno.EACCES, os.rename,
                   os.path.join(sub, "a"), os.path.join(sub, "z"))
        expect_err(errno.EACCES, os.link,
                   os.path.join(sub, "a"), os.path.join(sub, "hl"))
        expect_err(errno.EACCES, os.symlink, "a", os.path.join(sub, "sl"))
        assert os.path.exists(os.path.join(sub, "a"))
        eq(rd(os.path.join(sub, "a")), b"x", "denied mutate left file")
    finally:
        os.chmod(sub, 0o755)


@test
@serial
def perm_dir_no_x_search(d):
    _skip_root()
    sub = os.path.join(d, "sub")
    os.mkdir(sub)
    wr(os.path.join(sub, "child"), b"x")
    os.chmod(sub, 0o400)   # r--------
    cwd = os.getcwd()
    try:
        eq(set(os.listdir(sub)), {"child"}, "r lets listdir")
        expect_err(errno.EACCES, open, os.path.join(sub, "child"), "rb")
        expect_err(errno.EACCES, os.stat, os.path.join(sub, "child"))
        expect_err(errno.EACCES, os.chdir, sub)
        assert os.access(sub, os.R_OK)
        assert not os.access(sub, os.X_OK)
        assert not os.access(sub, os.W_OK)
    finally:
        os.chdir(cwd)
        os.chmod(sub, 0o755)


@test
def perm_dir_no_r_list(d):
    _skip_root()
    sub = os.path.join(d, "sub")
    os.mkdir(sub)
    wr(os.path.join(sub, "child"), b"hid")
    os.chmod(sub, 0o100)   # --x------
    try:
        expect_err(errno.EACCES, os.listdir, sub)
        eq(rd(os.path.join(sub, "child")), b"hid", "x lets named lookup")
        assert os.access(sub, os.X_OK)
        assert not os.access(sub, os.R_OK)
    finally:
        os.chmod(sub, 0o755)


@test
def perm_dir_wx_no_r(d):
    _skip_root()
    sub = os.path.join(d, "sub")
    os.mkdir(sub)
    os.chmod(sub, 0o300)   # -wx------
    try:
        wr(os.path.join(sub, "g"), b"ok")
        eq(rd(os.path.join(sub, "g")), b"ok", "wx lets create+open by name")
        expect_err(errno.EACCES, os.listdir, sub)
        os.unlink(os.path.join(sub, "g"))
    finally:
        os.chmod(sub, 0o755)


@test
def perm_dir_000_restore(d):
    _skip_root()
    sub = os.path.join(d, "sub")
    os.mkdir(sub)
    wr(os.path.join(sub, "f"), b"x")
    os.chmod(sub, 0o000)
    if not _root():
        expect_err(errno.EACCES, os.listdir, sub)
    os.chmod(sub, 0o755)   # owner chmod does not need x on the dir
    eq(rd(os.path.join(sub, "f")), b"x", "restored")


@test
def perm_unlink_mode_000_file(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.chmod(p, 0o000)
    os.unlink(p)   # unlink is parent wx, not file write
    assert not os.path.exists(p)


@test
def perm_nested_parent_no_x(d):
    _skip_root()
    a = os.path.join(d, "a")
    os.makedirs(os.path.join(a, "b"))
    wr(os.path.join(a, "b", "c"), b"deep")
    os.chmod(a, 0o000)
    try:
        expect_err(errno.EACCES, open, os.path.join(a, "b", "c"), "rb")
        expect_err(errno.EACCES, os.stat, os.path.join(a, "b"))
    finally:
        os.chmod(a, 0o755)


@test
def perm_rename_into_readonly_dir(d):
    _skip_root()
    src = os.path.join(d, "src")
    dst = os.path.join(d, "dst")
    os.mkdir(src)
    os.mkdir(dst)
    wr(os.path.join(src, "f"), b"x")
    os.chmod(dst, 0o555)
    try:
        expect_err(errno.EACCES, os.rename,
                   os.path.join(src, "f"), os.path.join(dst, "f"))
        assert os.path.exists(os.path.join(src, "f"))
    finally:
        os.chmod(dst, 0o755)


@test
def perm_symlink_target_denied(d):
    _skip_root()
    t = os.path.join(d, "t")
    wr(t, b"x")
    os.symlink("t", os.path.join(d, "sl"))
    os.chmod(t, 0o000)
    expect_err(errno.EACCES, open, os.path.join(d, "sl"), "rb")
    os.lstat(os.path.join(d, "sl"))   # lstat must still work


@test
def perm_hardlink_shares_mode(d):
    _skip_root()
    a = os.path.join(d, "a")
    b = os.path.join(d, "b")
    wr(a, b"shared")
    os.link(a, b)
    os.chmod(a, 0o444)
    eq(statmod.S_IMODE(os.stat(b).st_mode), 0o444, "mode via other name")
    expect_err(errno.EACCES, open, b, "wb")
    eq(rd(b), b"shared", "read via other name")


# ==========================================================================
# Crazy / adversarial file names
# ==========================================================================
CRAZY_NAMES = [
    "with space",
    "with\ttab",
    "with\nnewline",
    "dash-prefix",
    "-rf",
    "--help",
    "dot.mid.name",
    "trailingdot.",
    "..dots..",
    "...",
    "quote'single",
    'quote"double',
    "back\\slash",
    "semi;colon",
    "pipe|char",
    "amp&ersand",
    "dollar$sign",
    "star*char",
    "quest?char",
    "brack[et]",
    "brace{curly}",
    "paren(thesis)",
    "bang!char",
    "tilde~char",
    "percent%char",
    "caret^char",
    "plus+equals=",
    "comma,colon:char",
    "unicode-café-ümlaut-中文-🚀",
    "a" * 255,               # max single-component length on most fs
]


@test
def names_crazy_roundtrip(d):
    for n in CRAZY_NAMES:
        p = os.path.join(d, n)
        wr(p, b"data")
        eq(rd(p), b"data", "crazy name %r content" % n)
    listed = set(os.listdir(d))
    eq(listed, set(CRAZY_NAMES), "all crazy names listed")
    for n in CRAZY_NAMES:
        os.unlink(os.path.join(d, n))
    # The daemon's readdir hides the kernel's silly-rename artifacts
    # (.fuse_hidden<hex> = a file unlinked while still open), so a leftover
    # here is a REAL leaked dentry, not a transient kernel artifact.
    eq(os.listdir(d), [], "all crazy names removed: leftover %r" % os.listdir(d))


@test
def names_dash_prefix_terminal(d):
    # names that look like options must work via -- or ./
    wr(os.path.join(d, "-rf"), b"x")
    r = sh("cat -- -rf", cwd=d)
    eq(r.stdout, b"x", "cat -- -rf")
    sh("rm -- -rf", cwd=d)
    assert not os.path.exists(os.path.join(d, "-rf"))


@test
def names_too_long_component(d):
    p = os.path.join(d, "a" * 256)   # one byte over the 255 limit
    expect_err(errno.ENAMETOOLONG, wr, p, b"x")


@test
def names_reserved_dots(d):
    expect_err(errno.EEXIST, os.mkdir, os.path.join(d, "."))
    expect_err(errno.EEXIST, os.mkdir, os.path.join(d, ".."))


# Linux PATH_MAX is 4096 including the trailing NUL, so the longest
# pathname string is 4095 bytes. efs uses the same bound (EFS_MAX_PATH).
# Each component stays <= 255 (NAME_MAX).
_PATH_MAX_STR = 4095
_NAME_MAX = 255


def _mkdir_deep(d, max_abs):
    """Nest dirs under d until the abs path is as long as possible while
    leaving room for '/x' and staying < max_abs. Returns the abs dir."""
    cur = os.path.abspath(d)
    while True:
        room = max_abs - len(cur) - 1
        n = min(_NAME_MAX, room - 2)
        if n < 1:
            break
        nxt = os.path.join(cur, "a" * n)
        os.mkdir(nxt)
        cur = nxt
        if n < _NAME_MAX:
            break
    return cur


@test
def names_near_path_max(d):
    cur = _mkdir_deep(d, _PATH_MAX_STR)
    room = _PATH_MAX_STR - len(cur) - 1
    if room < 1 or room > _NAME_MAX:
        raise Fail("deepen left name room %d (prefix %d)" % (room, len(cur)))
    p = os.path.join(cur, "f" * room)
    if not (4000 <= len(p) <= _PATH_MAX_STR):
        raise Fail("constructed path %d bytes, want 4000..%d" %
                   (len(p), _PATH_MAX_STR))
    wr(p, b"near-path-max")
    eq(rd(p), b"near-path-max", "read back")
    eq(os.stat(p).st_size, 13, "size")
    if ("f" * room) not in os.listdir(cur):
        raise Fail("readdir missed long-path file")
    os.unlink(p)
    assert not os.path.exists(p)


@test
def names_near_path_max_dir(d):
    cur = _mkdir_deep(d, _PATH_MAX_STR)
    room = _PATH_MAX_STR - len(cur) - 1
    if room < 1 or room > _NAME_MAX:
        raise Fail("deepen left name room %d (prefix %d)" % (room, len(cur)))
    p = os.path.join(cur, "d" * room)
    if not (4000 <= len(p) <= _PATH_MAX_STR):
        raise Fail("constructed path %d bytes, want 4000..%d" %
                   (len(p), _PATH_MAX_STR))
    os.mkdir(p)
    assert os.path.isdir(p)
    os.rmdir(p)
    assert not os.path.exists(p)


@test
def names_path_too_long(d):
    cur = _mkdir_deep(d, _PATH_MAX_STR)
    room = (_PATH_MAX_STR + 1) - len(cur) - 1
    if room < 1:
        room = 1
    if room > _NAME_MAX:
        raise Fail("cannot push past PATH_MAX with one component (prefix %d)"
                   % len(cur))
    p = os.path.join(cur, "b" * room)
    if len(p) < _PATH_MAX_STR + 1:
        raise Fail("constructed path %d, want >= %d" %
                   (len(p), _PATH_MAX_STR + 1))
    expect_err(errno.ENAMETOOLONG, wr, p, b"x")
    expect_err(errno.ENAMETOOLONG, os.mkdir, p)


# ==========================================================================
# Strange names: raw bytes / unicode normalization / crazy dirs / deep nesting
# POSIX allows any byte in a name except NUL and '/'.
# ==========================================================================
@test
def names_control_and_high_bytes(d):
    # Raw-byte names: control chars, DEL, high/non-UTF8 bytes, whitespace-only.
    # Use bytes paths (os accepts them) so we are not limited to valid UTF-8.
    names = [
        b"ctrl-\x01\x02\x1f",
        b"del-\x7f",
        b"high-\x80\xff\xfe",
        b"mix- \t\x01\x7f\x80\xff",
        b" ",                  # single space
        b"   ",                # only spaces
        b" leading ",          # leading+trailing space
        b"trailing\t",         # trailing tab
        b"a",                  # single char
        b"\xe4\xb8\xad\xe6\x96\x87",  # valid UTF-8 (Chinese) as bytes
    ]
    bd = os.fsencode(d)
    for n in names:
        p = os.path.join(bd, n)
        with open(p, "wb") as f:
            f.write(b"x")
        with open(p, "rb") as f:
            eq(f.read(), b"x", "byte-name content %r" % n)
    listed = set(os.listdir(bd))
    for n in names:
        if n not in listed:
            raise Fail("byte-name %r missing from readdir" % n)
    for n in names:
        os.unlink(os.path.join(bd, n))
    eq(os.listdir(bd), [], "byte-names removed: leftover %r" % os.listdir(bd))


def _nfc_nfd_names():
    """Return (NFC, NFD) for the same word 'café'.

    The raw encodings differ and each normalizes to the other form. A name
    built as cafe + U+00E9 ('cafeé') is a different word and does not catch
    a filesystem that folds canonical equivalents together.
    """
    import unicodedata
    nfc = unicodedata.normalize("NFC", "caf\u00e9")
    nfd = unicodedata.normalize("NFD", nfc)
    if nfc == nfd or os.fsencode(nfc) == os.fsencode(nfd):
        raise Fail("test setup: NFC and NFD encodings are not distinct")
    if unicodedata.normalize("NFC", nfd) != nfc:
        raise Fail("test setup: NFD does not normalize back to NFC")
    if unicodedata.normalize("NFD", nfc) != nfd:
        raise Fail("test setup: NFC does not normalize to NFD")
    return nfc, nfd


@test
def names_unicode_nfc_nfd_coexist(d):
    # NFC (é = U+00E9) vs NFD (e + U+0301) of the same word must coexist.
    nfc, nfd = _nfc_nfd_names()
    wr(os.path.join(d, nfc), b"nfc")
    wr(os.path.join(d, nfd), b"nfd")
    eq(rd(os.path.join(d, nfc)), b"nfc", "NFC roundtrip")
    eq(rd(os.path.join(d, nfd)), b"nfd", "NFD roundtrip")
    eq(len(os.listdir(d)), 2, "NFC and NFD coexist as distinct names")


@test
def names_crazy_dirs(d):
    # Crazy names as DIRECTORIES: mkdir, a file inside, readdir, then remove.
    for n in CRAZY_NAMES:
        sub = os.path.join(d, n)
        os.mkdir(sub)
        wr(os.path.join(sub, "inner"), b"y")
        eq(rd(os.path.join(sub, "inner")), b"y", "file inside crazy dir %r" % n)
        eq(os.listdir(sub), ["inner"], "readdir crazy dir %r" % n)
    for n in CRAZY_NAMES:
        sub = os.path.join(d, n)
        os.unlink(os.path.join(sub, "inner"))
        rmdir_rideout_sillyrename(sub)
    eq(os.listdir(d), [], "crazy dirs removed: leftover %r" % os.listdir(d))


@test
def dir_deep_nesting_beyond_64(d):
    # EFS_LOOKUP_PATH_MAX_DEPTH is 64 (the batched-ancestor cap). Deeper paths
    # must still work via the client's no-ancestor retry / per-component walk.
    depth = 100
    cur = d
    for _ in range(depth):
        cur = os.path.join(cur, "d")
        os.mkdir(cur)
    p = os.path.join(cur, "leaf")
    wr(p, b"deep")
    eq(rd(p), b"deep", "read at depth ~%d" % depth)
    eq(os.stat(p).st_size, 4, "stat at depth")
    eq(os.listdir(cur), ["leaf"], "readdir at depth")


# ==========================================================================
# File content: seeded random + strange bytes (byte-exact round-trip)
# ==========================================================================
@test
def content_random_roundtrip(d):
    # Seeded random data at sizes around the 128 KiB chunk boundary and
    # multi-chunk. Deterministic seed -> we know the exact expected bytes.
    sizes = [1, 2, 63, 64, 65, 4096, 65535, 131071, 131072, 131073,
             262144, 400000]
    for i, n in enumerate(sizes):
        data = rand_bytes("roundtrip-%d" % i, n)
        p = os.path.join(d, "f%d" % i)
        wr(p, data)
        eq(os.path.getsize(p), n, "size n=%d" % n)
        eq(rd(p), data, "random content roundtrip n=%d" % n)


@test
def content_all_byte_values(d):
    # Every byte value 0x00..0xff, in order and shuffled, must round-trip.
    p = os.path.join(d, "allbytes")
    data = bytes(range(256)) * 4
    wr(p, data)
    eq(rd(p), data, "all 256 byte values in order")
    shuf = rand_bytes("shuffled", 8192)
    wr(p, shuf)
    eq(rd(p), shuf, "shuffled random bytes roundtrip")


@test
def content_whitespace_and_line_endings(d):
    p = os.path.join(d, "ws")
    data = (b"  leading spaces\n"
            b"trailing spaces   \n"
            b"\t\ttabs\t\t\n"
            b"blank lines\n\n\n\n"
            b"crlf\r\nline\r\n"
            b"lone cr\rline\r"
            b"mixed \t \t spaces \t\n"
            b"no trailing newline")
    wr(p, data)
    eq(rd(p), data, "whitespace/line-ending content")
    wr(p, b"     ")
    eq(rd(p), b"     ", "only-spaces file")
    wr(p, b"\n\n\n")
    eq(rd(p), b"\n\n\n", "only-newlines file")
    wr(p, b" \t\r\n ")
    eq(rd(p), b" \t\r\n ", "only-whitespace mix")


@test
def content_nul_and_control(d):
    p = os.path.join(d, "bin")
    data = bytes(range(0x00, 0x20)) * 8   # NUL + all control chars
    wr(p, data)
    eq(rd(p), data, "NUL + control bytes")
    eq(os.path.getsize(p), len(data), "binary size")


@test
def content_random_overwrite_append(d):
    base = bytearray(rand_bytes("ov-base", 65536))
    p = os.path.join(d, "f")
    wr(p, bytes(base))
    patch = rand_bytes("ov-patch", 10000)     # random overwrite of the middle
    fd = os.open(p, os.O_RDWR)
    os.pwrite(fd, patch, 20000)
    os.close(fd)
    base[20000:20000 + 10000] = patch
    eq(rd(p), bytes(base), "random overwrite middle merged")
    tail = rand_bytes("ov-tail", 5000)        # random append
    with open(p, "ab") as f:
        f.write(tail)
    base += tail
    eq(rd(p), bytes(base), "random append merged")
    eq(os.path.getsize(p), len(base), "size after overwrite+append")


@test
def content_random_large_multichunk(d):
    # 2 MiB of seeded random spanning many 128 KiB chunks; compare hashes so
    # we don't hold two full copies. Same seed -> same hash on any client.
    n = 2 * 1024 * 1024
    data = rand_bytes("large-2m", n)
    p = os.path.join(d, "big")
    wr(p, data)
    eq(os.path.getsize(p), n, "large size")
    eq(hashlib.sha256(rd(p)).hexdigest(), hashlib.sha256(data).hexdigest(),
       "large random content sha256")


# ==========================================================================
# Error / "impossible" operations (must fail with a specific errno)
# ==========================================================================
@test
def err_mkdir_existing(d):
    os.mkdir(os.path.join(d, "sub"))
    expect_err(errno.EEXIST, os.mkdir, os.path.join(d, "sub"))


@test
def err_rmdir_nonempty(d):
    os.mkdir(os.path.join(d, "sub"))
    wr(os.path.join(d, "sub/f"), b"x")
    expect_err(errno.ENOTEMPTY, os.rmdir, os.path.join(d, "sub"))


@test
def err_rmdir_a_file(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    expect_err(errno.ENOTDIR, os.rmdir, p)


@test
def err_open_dir_for_write(d):
    os.mkdir(os.path.join(d, "sub"))
    expect_err(errno.EISDIR, open, os.path.join(d, "sub"), "wb")


@test
def err_read_from_dir(d):
    os.mkdir(os.path.join(d, "sub"))
    fd = os.open(os.path.join(d, "sub"), os.O_RDONLY)
    try:
        expect_err(errno.EISDIR, os.read, fd, 10)
    finally:
        os.close(fd)


@test
def err_create_excl_existing(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    expect_err(errno.EEXIST, os.open, p, os.O_CREAT | os.O_EXCL | os.O_WRONLY)


@test
def err_rename_dir_into_itself(d):
    os.makedirs(os.path.join(d, "a/b"))
    expect_err((errno.EINVAL, errno.EBUSY), os.rename,
               os.path.join(d, "a"), os.path.join(d, "a/b/c"))


@test
def err_hardlink_to_dir(d):
    os.mkdir(os.path.join(d, "sub"))
    expect_err((errno.EPERM, errno.EEXIST, errno.EACCES), os.link,
               os.path.join(d, "sub"), os.path.join(d, "hl"))


@test
def err_unlink_nonexistent(d):
    expect_err(errno.ENOENT, os.unlink, os.path.join(d, "ghost"))


@test
def err_hardlink_nonexistent_src(d):
    expect_err(errno.ENOENT, os.link,
               os.path.join(d, "ghost"), os.path.join(d, "hl"))


@test
def err_write_readonly_file(d):
    if os.geteuid() == 0:
        return  # root bypasses permission checks; skip
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.chmod(p, 0o444)
    expect_err(errno.EACCES, open, p, "wb")


@test
def err_symlink_loop(d):
    os.symlink("b", os.path.join(d, "a"))
    os.symlink("a", os.path.join(d, "b"))
    expect_err(errno.ELOOP, os.stat, os.path.join(d, "a"))


@test
def err_create_under_a_file(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    expect_err(errno.ENOTDIR, wr, os.path.join(p, "child"), b"x")


@test
@serial
def err_chdir_into_file(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    expect_err(errno.ENOTDIR, os.chdir, p)


@test
def err_stat_nonexistent(d):
    expect_err(errno.ENOENT, os.stat, os.path.join(d, "ghost"))


@test
def err_unlink_a_directory(d):
    os.mkdir(os.path.join(d, "sub"))
    expect_err((errno.EISDIR, errno.EPERM), os.unlink, os.path.join(d, "sub"))


@test
def err_rmdir_dot_dotdot(d):
    os.mkdir(os.path.join(d, "sub"))
    expect_err((errno.EINVAL, errno.ENOTEMPTY, errno.EBUSY),
               os.rmdir, os.path.join(d, "sub", "."))
    expect_err((errno.ENOTEMPTY, errno.EINVAL, errno.EBUSY),
               os.rmdir, os.path.join(d, "sub", ".."))


# ==========================================================================
# fsync / fdatasync / durability surface
# ==========================================================================
@test
def fsync_write(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    os.write(fd, b"durable")
    os.fsync(fd)
    os.close(fd)
    eq(rd(p), b"durable", "fsync'd data readable")


@test
def fdatasync_write(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    os.write(fd, b"x" * 1000)
    os.fdatasync(fd)
    os.close(fd)
    eq(os.path.getsize(p), 1000, "fdatasync size")


@test
def fsync_dir(d):
    sub = os.path.join(d, "sub")
    os.mkdir(sub)
    wr(os.path.join(sub, "f"), b"x")
    fd = os.open(sub, os.O_RDONLY)
    os.fsync(fd)                # fsync the directory entry
    os.close(fd)


# ==========================================================================
# unlink-open-file semantics
# ==========================================================================
@test
def unlink_open_file(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    os.write(fd, b"open-data")
    os.unlink(p)                 # unlink while open
    assert not os.path.exists(p)
    os.lseek(fd, 0, os.SEEK_SET)
    eq(os.read(fd, 9), b"open-data", "open fd still readable after unlink")
    os.write(fd, b"!")           # and writable
    os.close(fd)


# ==========================================================================
# File locking
# ==========================================================================
@test
def flock_basic(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    fd = os.open(p, os.O_RDWR)
    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    fcntl.flock(fd, fcntl.LOCK_UN)
    os.close(fd)


@test
def fcntl_lock_basic(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    fd = os.open(p, os.O_RDWR)
    fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    fcntl.lockf(fd, fcntl.LOCK_UN)
    os.close(fd)


@test
def flock_shared_then_exclusive(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    fd1 = os.open(p, os.O_RDWR)
    fd2 = os.open(p, os.O_RDWR)
    try:
        fcntl.flock(fd1, fcntl.LOCK_SH | fcntl.LOCK_NB)
        fcntl.flock(fd2, fcntl.LOCK_SH | fcntl.LOCK_NB)
        try:
            fcntl.flock(fd2, fcntl.LOCK_UN)
            fcntl.flock(fd2, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as e:
            if e.errno in (errno.EAGAIN, errno.EACCES):
                return
            raise Fail("upgrade to LOCK_EX: %s" % e)
        raise Fail("LOCK_EX taken while another fd holds LOCK_SH")
    finally:
        try:
            fcntl.flock(fd1, fcntl.LOCK_UN)
        except OSError:
            pass
        os.close(fd1)
        os.close(fd2)


def _wait_for(path, seconds, what):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if os.path.exists(path):
            return
        time.sleep(0.02)
    raise Fail(what)


@test
def flock_two_proc_exclusive(d):
    """Another process holds LOCK_EX until this process asks it to release."""
    p = os.path.join(d, "f")
    ready = os.path.join(d, ".flock-ready")
    release = os.path.join(d, ".flock-release")
    wr(p, b"x")
    snippet = (
        "import fcntl, os, sys, time\n"
        "fd = os.open(sys.argv[1], os.O_RDWR)\n"
        "fcntl.flock(fd, fcntl.LOCK_EX)\n"
        "open(sys.argv[2], 'w').write('1\\n')\n"
        "deadline = time.monotonic() + 10\n"
        "while not os.path.exists(sys.argv[3]):\n"
        "    if time.monotonic() > deadline:\n"
        "        sys.exit(3)\n"
        "    time.sleep(0.02)\n"
        "fcntl.flock(fd, fcntl.LOCK_UN)\n"
        "os.close(fd)\n"
    )
    child = subprocess.Popen(
        [sys.executable, "-c", snippet, p, ready, release])
    try:
        try:
            _wait_for(ready, 5.0, "child did not acquire LOCK_EX")
        except Fail:
            child.kill()
            raise
        fd = os.open(p, os.O_RDWR)
        try:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as e:
                if e.errno not in (errno.EAGAIN, errno.EACCES):
                    raise Fail("parent lock errno %s" % e)
            else:
                fcntl.flock(fd, fcntl.LOCK_UN)
                raise Fail("parent got LOCK_EX|NB while child held LOCK_EX")
            open(release, "w").close()
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except OSError as e:
                    if e.errno not in (errno.EAGAIN, errno.EACCES):
                        raise Fail("parent relock errno %s" % e)
                    time.sleep(0.02)
                    continue
                fcntl.flock(fd, fcntl.LOCK_UN)
                break
            else:
                raise Fail("parent did not acquire LOCK_EX after child release")
        finally:
            os.close(fd)
    finally:
        child.wait()


@test
def fcntl_byte_range_lock(d):
    """POSIX record locks belong to the process, and conflict across processes.

    Same-process overlap must succeed (the second lock merges). Another
    process's overlap must fail with EAGAIN/EACCES, an adjacent range must
    succeed, and a blocked waiter must acquire only after an explicit unlock.
    """
    p = os.path.join(d, "f")
    wr(p, b"x" * 100)
    fd1 = os.open(p, os.O_RDWR)
    fd2 = os.open(p, os.O_RDWR)
    try:
        fcntl.lockf(fd1, fcntl.LOCK_EX | fcntl.LOCK_NB, 50, 0)
        try:
            fcntl.lockf(fd2, fcntl.LOCK_EX | fcntl.LOCK_NB, 20, 10)
        except OSError as e:
            if e.errno in (errno.EAGAIN, errno.EACCES):
                raise Fail("same-process overlapping lockf conflicted "
                           "(record locks are per process)")
            raise Fail("same-process overlapping lockf: %s" % e)
        fcntl.lockf(fd2, fcntl.LOCK_UN, 20, 10)
        try:
            fcntl.lockf(fd2, fcntl.LOCK_EX | fcntl.LOCK_NB, 10, 50)
        except OSError as e:
            raise Fail("same-process adjacent lockf: %s" % e)
        fcntl.lockf(fd2, fcntl.LOCK_UN, 10, 50)
        fcntl.lockf(fd1, fcntl.LOCK_UN, 50, 0)
    finally:
        os.close(fd1)
        os.close(fd2)

    held = os.path.join(d, ".range-held")
    release = os.path.join(d, ".range-release")
    holder = (
        "import fcntl, os, sys, time\n"
        "fd = os.open(sys.argv[1], os.O_RDWR)\n"
        "fcntl.lockf(fd, fcntl.LOCK_EX, 50, 0)\n"
        "open(sys.argv[2], 'w').write('held\\n')\n"
        "deadline = time.monotonic() + 10\n"
        "while not os.path.exists(sys.argv[3]):\n"
        "    if time.monotonic() > deadline:\n"
        "        sys.exit(3)\n"
        "    time.sleep(0.02)\n"
        "fcntl.lockf(fd, fcntl.LOCK_UN, 50, 0)\n"
        "os.close(fd)\n"
    )
    child = subprocess.Popen(
        [sys.executable, "-c", holder, p, held, release])
    fd = os.open(p, os.O_RDWR)
    try:
        try:
            _wait_for(held, 5.0, "other process did not acquire [0, 50)")
        except Fail:
            child.kill()
            raise
        try:
            fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB, 20, 10)
        except OSError as e:
            if e.errno not in (errno.EAGAIN, errno.EACCES):
                raise Fail("cross-process overlap errno %s" % e)
        else:
            fcntl.lockf(fd, fcntl.LOCK_UN, 20, 10)
            raise Fail("cross-process overlapping lockf succeeded")
        try:
            fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB, 10, 50)
        except OSError as e:
            raise Fail("adjacent range vs other process: %s" % e)
        fcntl.lockf(fd, fcntl.LOCK_UN, 10, 50)
        open(release, "w").close()
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            try:
                fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB, 50, 0)
            except OSError as e:
                if e.errno not in (errno.EAGAIN, errno.EACCES):
                    raise Fail("relock after release errno %s" % e)
                time.sleep(0.02)
                continue
            break
        else:
            raise Fail("did not acquire [0, 50) after the holder released")

        # Parent now holds [0, 50). A child blocks in lockf until we unlock.
        reached = os.path.join(d, ".waiter-reached")
        acquired = os.path.join(d, ".waiter-acquired")
        waiter = (
            "import fcntl, os, sys\n"
            "fd = os.open(sys.argv[1], os.O_RDWR)\n"
            "open(sys.argv[2], 'w').write('reached\\n')\n"
            "fcntl.lockf(fd, fcntl.LOCK_EX, 20, 10)\n"
            "open(sys.argv[3], 'w').write('got\\n')\n"
            "fcntl.lockf(fd, fcntl.LOCK_UN, 20, 10)\n"
            "os.close(fd)\n"
        )
        blocked = subprocess.Popen(
            [sys.executable, "-c", waiter, p, reached, acquired])
        try:
            _wait_for(reached, 5.0, "waiter did not start")
            confirm = time.monotonic() + 0.3
            while time.monotonic() < confirm:
                if os.path.exists(acquired):
                    raise Fail("waiter acquired the range while it was held")
                time.sleep(0.02)
            fcntl.lockf(fd, fcntl.LOCK_UN, 50, 0)
            _wait_for(acquired, 5.0, "waiter did not acquire after unlock")
        finally:
            if blocked.poll() is None:
                blocked.kill()
            blocked.wait()
    finally:
        try:
            fcntl.lockf(fd, fcntl.LOCK_UN, 50, 0)
        except OSError:
            pass
        os.close(fd)
        if child.poll() is None:
            child.kill()
        child.wait()


# ==========================================================================
# mmap (may be unsupported by the FUSE client — report, don't hard-fail)
# ==========================================================================
@test
def mmap_write_read(d):
    import mmap as mmapmod
    p = os.path.join(d, "f")
    wr(p, b"\x00" * 4096)
    fd = os.open(p, os.O_RDWR)
    try:
        m = mmapmod.mmap(fd, 4096)
    except OSError as e:
        os.close(fd)
        raise Fail("mmap unsupported: %s" % e, soft=True)
    m[0:5] = b"mmap!"
    m.flush()
    m.close()
    os.close(fd)
    eq(rd(p)[:5], b"mmap!", "mmap write visible")


@test
def mmap_private_not_visible(d):
    import mmap as mmapmod
    p = os.path.join(d, "f")
    wr(p, b"\x00" * 4096)
    fd = os.open(p, os.O_RDWR)
    try:
        m = mmapmod.mmap(fd, 4096, access=mmapmod.ACCESS_COPY)
    except OSError as e:
        os.close(fd)
        raise Fail("mmap private unsupported: %s" % e, soft=True)
    m[0:3] = b"xyz"
    m.close()
    os.close(fd)
    eq(rd(p)[:3], b"\x00\x00\x00", "MAP_PRIVATE must not dirty the file")


# ==========================================================================
# statfs
# ==========================================================================
@test
def statfs_sane(d):
    sv = os.statvfs(d)
    assert sv.f_blocks >= 0, "blocks"
    assert sv.f_bsize > 0, "bsize"
    assert sv.f_bfree <= sv.f_blocks, "bfree <= blocks"
    r = sh("df .", cwd=d)
    assert r.returncode == 0, "df works"


# ==========================================================================
# Concurrency
# ==========================================================================
@test
def concurrent_creates_same_dir(d):
    import threading
    errors = []

    def worker(i):
        try:
            for j in range(20):
                wr(os.path.join(d, "t%d-%d" % (i, j)), b"x")
        except Exception as e:  # noqa: BLE001
            errors.append(e)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errors:
        raise Fail("concurrent create errors: %s" % errors[0])
    eq(len(os.listdir(d)), 8 * 20, "all concurrent creates present")


def _append_record(writer, seq):
    return ("W%sS%04d\n" % (writer, seq)).encode()


def _write_full(fd, rec):
    n = os.write(fd, rec)
    if n != len(rec):
        raise Fail("short append write %d/%d" % (n, len(rec)))
    return n


def _check_append_log(data, writers, count):
    """Every writer/sequence record once, intact. A matching line count is not enough."""
    if not data.endswith(b"\n"):
        raise Fail("torn trailing append record: %r" % data[-24:])
    lines = data.splitlines()
    want = [_append_record(w, i)[:-1] for w in writers for i in range(count)]
    if len(lines) != len(want):
        raise Fail("append count %d, want %d" % (len(lines), len(want)))
    got = {}
    for ln in lines:
        got[ln] = got.get(ln, 0) + 1
    for rec in want:
        n = got.get(rec, 0)
        if n != 1:
            raise Fail("append record %r count %d, want 1" % (rec, n))
    extra = [ln for ln in got if ln not in want]
    if extra:
        raise Fail("unexpected append record %r" % extra[0])


@test
@budget(30)
def concurrent_appends(d):
    import threading
    p = os.path.join(d, "f")
    wr(p, b"")
    errors = []
    nwriters, count = 4, 50

    def worker(i):
        try:
            for j in range(count):
                rec = _append_record(i, j)
                with open(p, "ab") as f:
                    _write_full(f.fileno(), rec)
        except Exception as e:  # noqa: BLE001
            errors.append(e)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(nwriters)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errors:
        raise Fail("append worker: %s" % errors[0])
    _check_append_log(rd(p), list(range(nwriters)), count)


@test
@budget(30)
def concurrent_appends_two_proc(d):
    p = os.path.join(d, "f")
    wr(p, b"")
    nwriters, count = 3, 40
    snippet = (
        "import os,sys\n"
        "fd=os.open(sys.argv[1], os.O_WRONLY|os.O_APPEND)\n"
        "writer=sys.argv[2]\n"
        "n=int(sys.argv[3])\n"
        "for i in range(n):\n"
        "    rec=('W%sS%04d\\n'%(writer,i)).encode()\n"
        "    got=os.write(fd, rec)\n"
        "    if got!=len(rec):\n"
        "        sys.exit(2)\n"
        "os.close(fd)\n"
    )
    procs = [
        subprocess.Popen([sys.executable, "-c", snippet, p, str(i), str(count)])
        for i in range(nwriters)
    ]
    for pr in procs:
        if pr.wait() != 0:
            raise Fail("append child rc=%d" % pr.returncode)
    _check_append_log(rd(p), list(range(nwriters)), count)


@test
def concurrent_writes_disjoint(d):
    import threading
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    os.ftruncate(fd, 8192)
    os.close(fd)
    errors = []

    def worker(off, byte):
        try:
            f = os.open(p, os.O_RDWR)
            os.pwrite(f, byte * 4096, off)
            os.close(f)
        except Exception as e:  # noqa: BLE001
            errors.append(e)

    t0 = threading.Thread(target=worker, args=(0, b"A"))
    t1 = threading.Thread(target=worker, args=(4096, b"B"))
    t0.start()
    t1.start()
    t0.join()
    t1.join()
    if errors:
        raise Fail("disjoint write: %s" % errors[0])
    data = rd(p)
    eq(data[:4096], b"A" * 4096, "low range")
    eq(data[4096:8192], b"B" * 4096, "high range")


@test
@budget(30)
def concurrent_create_unlink_two_proc(d):
    snippet = (
        "import os,sys,time\n"
        "d=sys.argv[1]; tag=sys.argv[2]\n"
        "for i in range(30):\n"
        "    p=os.path.join(d,'c%s-%d'%(tag,i))\n"
        "    fd=os.open(p, os.O_CREAT|os.O_WRONLY, 0o644)\n"
        "    os.write(fd,b'x'); os.close(fd)\n"
        "    os.unlink(p)\n"
    )
    procs = [
        subprocess.Popen([sys.executable, "-c", snippet, d, str(i)],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for i in range(3)
    ]
    for pr in procs:
        _out, err = pr.communicate()
        if pr.returncode != 0:
            raise Fail("create/unlink child rc=%d: %s"
                       % (pr.returncode, err.decode(errors="replace")[-300:]))
    leftover = [n for n in os.listdir(d) if n.startswith("c")]
    eq(leftover, [], "no leftover create/unlink names")


@test
def concurrent_write_and_readdir(d):
    import threading
    wr(os.path.join(d, "seed"), b"x")
    errors = []

    def writer():
        try:
            for i in range(40):
                wr(os.path.join(d, "w%02d" % i), b"x")
        except Exception as e:  # noqa: BLE001
            errors.append(e)

    def reader():
        try:
            for _ in range(40):
                os.listdir(d)
        except Exception as e:  # noqa: BLE001
            errors.append(e)

    tw = threading.Thread(target=writer)
    tr = threading.Thread(target=reader)
    tw.start()
    tr.start()
    tw.join()
    tr.join()
    if errors:
        raise Fail("write+readdir: %s" % errors[0])
    eq(len([n for n in os.listdir(d) if n.startswith("w")]), 40, "all writes listed")


# ==========================================================================
# Virtual .stats / .find (efs FUSE extras; skip if feature off)
# ==========================================================================
@test
def virt_stats_readable(d):
    wr(os.path.join(d, "a"), b"x")
    p = os.path.join(d, ".stats")
    if not os.path.exists(p):
        raise Fail(".stats absent (feature off?)", soft=True)
    st = os.lstat(p)
    assert statmod.S_ISREG(st.st_mode), ".stats is a regular file"
    text = rd(p)
    if len(text) == 0:
        raise Fail(".stats empty")
    expect_err((errno.EPERM, errno.EACCES, errno.ENOENT, errno.EIO),
               os.unlink, p)


@test
def virt_find_query(d):
    wr(os.path.join(d, "report.txt"), b"x")
    finddir = os.path.join(d, ".find")
    if not os.path.isdir(finddir) and not os.path.exists(finddir):
        raise Fail(".find absent (feature off?)", soft=True)
    q = os.path.join(d, ".find", "*report*")
    try:
        out = rd(q)
    except OSError as e:
        raise Fail(".find query: %s" % e, soft=True)
    if b"report.txt" not in out:
        raise Fail(".find *report* missed report.txt: %r" % out[:200])


@test
def virt_find_not_a_real_dir(d):
    finddir = os.path.join(d, ".find")
    if not os.path.exists(finddir) and not os.path.lexists(finddir):
        raise Fail(".find absent (feature off?)", soft=True)
    expect_err((errno.EPERM, errno.EACCES, errno.ENOENT, errno.EEXIST,
                errno.ENOTDIR, errno.EIO),
               wr, os.path.join(d, ".find", "not-a-create"), b"x")


# ==========================================================================
# Optional POSIX (success or EOPNOTSUPP — both OK)
# ==========================================================================
@test
def opt_fallocate(d):
    # W26: ll_fallocate. The raw fallocate(2) syscall on an O_DIRECT fd
    # has no glibc fallback (posix_fallocate's zero-fill pwrite is what
    # made a plain fd "pass" before the handler existed, and on O_DIRECT
    # that pwrite is misaligned → EINVAL). Mode 0 past EOF extends the
    # size with no data written; KEEP_SIZE inside the file is a no-op
    # success; PUNCH_HOLE is EOPNOTSUPP so callers can fall back.
    import ctypes
    libc = ctypes.CDLL(None, use_errno=True)
    libc.fallocate.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int64,
                               ctypes.c_int64]
    libc.fallocate.restype = ctypes.c_int

    def falloc(fd, mode, off, ln):
        r = libc.fallocate(fd, mode, off, ln)
        return 0 if r == 0 else ctypes.get_errno()

    FALLOC_FL_KEEP_SIZE = 0x01
    FALLOC_FL_PUNCH_HOLE = 0x02
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR | os.O_DIRECT, 0o644)
    try:
        e = falloc(fd, 0, 0, 1 << 20)
        if e in (errno.EOPNOTSUPP, errno.ENOTSUP, errno.ENOSYS):
            raise Fail("fallocate unsupported: %s" % os.strerror(e), soft=True)
        if e:
            raise Fail("fallocate(0, 0, 1M): %s" % os.strerror(e))
        eq(os.fstat(fd).st_size, 1 << 20, "size after fallocate extend")
        # Inside the file: nothing to do, size unchanged.
        eq(falloc(fd, 0, 4096, 4096), 0, "fallocate inside the file")
        eq(falloc(fd, FALLOC_FL_KEEP_SIZE, 0, 4096), 0,
           "fallocate KEEP_SIZE inside the file")
        eq(os.fstat(fd).st_size, 1 << 20, "size unchanged by inside calls")
        # Other modes: supported (0, the XFS baseline) or EOPNOTSUPP (efs,
        # so callers can fall back) — never EINVAL or another error.
        e = falloc(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 4096)
        if e not in (0, errno.EOPNOTSUPP, errno.ENOTSUP):
            raise Fail("punch-hole returned %s, want 0 or EOPNOTSUPP" %
                       os.strerror(e))
        # KEEP_SIZE past EOF would need a reservation efs does not have.
        e = falloc(fd, FALLOC_FL_KEEP_SIZE, 1 << 20, 4096)
        if e not in (0, errno.EOPNOTSUPP, errno.ENOTSUP):
            raise Fail("KEEP_SIZE past EOF returned %s, want 0 or EOPNOTSUPP" %
                       os.strerror(e))
        eq(os.fstat(fd).st_size, 1 << 20, "size unchanged by KEEP_SIZE")
    finally:
        os.close(fd)
    # The extended range is a hole: zeros, and the file still stats 1 MiB.
    eq(os.stat(p).st_size, 1 << 20, "size after close")
    with open(p, "rb") as f:
        data = f.read()
    eq(len(data), 1 << 20, "read length of the extended file")
    if data.count(b"\0") != len(data):
        raise Fail("extended range is not all zeros")


@test
def opt_xattr(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    try:
        os.setxattr(p, "user.efs", b"1")
        eq(os.getxattr(p, "user.efs"), b"1", "xattr roundtrip")
    except OSError as e:
        # EACCES is a permission failure, not "xattr is unsupported".
        if e.errno in (errno.EOPNOTSUPP, errno.ENOTSUP):
            raise Fail("xattr unsupported: %s" % e, soft=True)
        raise Fail("xattr: %s" % e)


@test
def opt_seek_hole_data(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    os.pwrite(fd, b"END", 1 << 20)
    try:
        hole = os.lseek(fd, 0, os.SEEK_HOLE)
        data = os.lseek(fd, 0, os.SEEK_DATA)
    except (OSError, AttributeError) as e:
        os.close(fd)
        if isinstance(e, AttributeError) or getattr(e, "errno", None) in (
                errno.EINVAL, errno.ENOTSUP, errno.EOPNOTSUPP):
            raise Fail("SEEK_HOLE/DATA unsupported: %s" % e, soft=True)
        raise Fail("SEEK_HOLE/DATA: %s" % e)
    os.close(fd)
    if hole != 0:
        raise Fail("SEEK_HOLE from 0 got %d, want 0 (leading hole)" % hole)
    if data < (1 << 20):
        raise Fail("SEEK_DATA from 0 got %d, want >= 1MiB" % data)


@test
def opt_copy_file_range(d):
    src = os.path.join(d, "src")
    dst = os.path.join(d, "dst")
    wr(src, b"ABCDEFGH")
    fd_s = os.open(src, os.O_RDONLY)
    fd_d = os.open(dst, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        n = os.copy_file_range(fd_s, fd_d, 8)
    except OSError as e:
        os.close(fd_s)
        os.close(fd_d)
        if e.errno in (errno.EOPNOTSUPP, errno.ENOTSUP, errno.EXDEV, errno.EINVAL):
            raise Fail("copy_file_range unsupported: %s" % e, soft=True)
        raise Fail("copy_file_range: %s" % e)
    os.close(fd_s)
    os.close(fd_d)
    if n != 8 or rd(dst) != b"ABCDEFGH":
        raise Fail("copy_file_range n=%s dst=%r" % (n, rd(dst)))


@test
def opt_openat(d):
    os.mkdir(os.path.join(d, "sub"))
    wr(os.path.join(d, "sub", "f"), b"at")
    dirfd = os.open(os.path.join(d, "sub"), os.O_RDONLY)
    try:
        fd = os.open("f", os.O_RDONLY, dir_fd=dirfd)
        eq(os.read(fd, 2), b"at", "openat")
        os.close(fd)
        os.unlink("f", dir_fd=dirfd)
    finally:
        os.close(dirfd)
    assert not os.path.exists(os.path.join(d, "sub", "f"))


@test
def flock_second_fd_exclusive(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    fd1 = os.open(p, os.O_RDWR)
    fd2 = os.open(p, os.O_RDWR)
    try:
        fcntl.flock(fd1, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            fcntl.flock(fd2, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as e:
            if e.errno in (errno.EAGAIN, errno.EACCES):
                return
            raise Fail("second LOCK_EX errno %s" % e)
        raise Fail("second fd took LOCK_EX|NB while first held it "
                   "(locks look like no-ops)")
    finally:
        fcntl.flock(fd1, fcntl.LOCK_UN)
        os.close(fd1)
        os.close(fd2)


# ==========================================================================
# O_DIRECT, vectored I/O, packed→chunked grow, two-fd visibility
# ==========================================================================
_DIO_ALIGN = 4096


def _dio_mmap(n):
    """Page-aligned anonymous buffer for O_DIRECT."""
    if n < 1:
        raise Fail("dio mmap size %d" % n)
    return mmap.mmap(-1, n)


def _dio_write(fd, data, off=None):
    m = _dio_mmap(len(data))
    m[:] = data
    try:
        if off is None:
            return os.writev(fd, [m])
        if hasattr(os, "pwritev"):
            return os.pwritev(fd, [m], off)
        os.lseek(fd, off, os.SEEK_SET)
        return os.writev(fd, [m])
    finally:
        m.close()


def _dio_read(fd, n, off=0):
    m = _dio_mmap(n)
    try:
        if hasattr(os, "preadv"):
            k = os.preadv(fd, [m], off)
        else:
            os.lseek(fd, off, os.SEEK_SET)
            k = os.readv(fd, [m])
        return bytes(m[:k])
    finally:
        m.close()


@test
def direct_aligned_rdwr(d):
    p = os.path.join(d, "f")
    payload = b"D" * _DIO_ALIGN
    fd = os.open(p, os.O_CREAT | os.O_RDWR | os.O_DIRECT, 0o644)
    try:
        _dio_write(fd, payload)
        os.fsync(fd)
        eq(_dio_read(fd, _DIO_ALIGN, 0), payload, "O_DIRECT same-fd read")
    finally:
        os.close(fd)
    fd2 = os.open(p, os.O_RDONLY | os.O_DIRECT)
    try:
        eq(_dio_read(fd2, _DIO_ALIGN, 0), payload, "O_DIRECT reopen read")
    finally:
        os.close(fd2)


@test
def direct_unaligned_einval(d):
    p = os.path.join(d, "f")
    wr(p, b"x" * _DIO_ALIGN)
    fd = os.open(p, os.O_RDWR | os.O_DIRECT)
    try:
        expect_err(errno.EINVAL, os.write, fd, b"short")
    finally:
        os.close(fd)


@test
def direct_rmw_4k_in_chunk(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR | os.O_DIRECT, 0o644)
    try:
        _dio_write(fd, b"A" * _CHUNK)
        os.fsync(fd)
        _dio_write(fd, b"B" * _DIO_ALIGN, off=8192)
        os.fsync(fd)
        got = _dio_read(fd, _CHUNK, 0)
    finally:
        os.close(fd)
    eq(got[:8192], b"A" * 8192, "prefix before 4k patch")
    eq(got[8192:8192 + _DIO_ALIGN], b"B" * _DIO_ALIGN, "4k patch")
    eq(got[8192 + _DIO_ALIGN:], b"A" * (_CHUNK - 8192 - _DIO_ALIGN),
       "suffix after 4k patch")


@test
def packed_then_grow_past_chunk(d):
    """Small (packed) file must keep its prefix after growing past 128 KiB."""
    p = os.path.join(d, "f")
    prefix = b"PACKED-HEAD"
    wr(p, prefix)
    fd = os.open(p, os.O_RDWR)
    try:
        os.fsync(fd)
        os.lseek(fd, 0, os.SEEK_END)
        os.write(fd, b"M" * _CHUNK)
        os.fsync(fd)
    finally:
        os.close(fd)
    data = rd(p)
    eq(data[:len(prefix)], prefix, "packed prefix survived grow")
    eq(data[len(prefix):], b"M" * _CHUNK, "grown tail")


@test
def writev_readv_chunk_straddle(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        n = os.writev(fd, [b"A" * (_CHUNK - 3), b"XYZ", b"B" * 16])
        eq(n, _CHUNK - 3 + 3 + 16, "writev n")
        os.lseek(fd, _CHUNK - 3, os.SEEK_SET)
        bufs = [bytearray(3), bytearray(16)]
        n = os.readv(fd, bufs)
        eq(n, 19, "readv n")
        eq(bytes(bufs[0]), b"XYZ", "readv straddle")
        eq(bytes(bufs[1]), b"B" * 16, "readv tail")
    finally:
        os.close(fd)


@test
def zero_length_write(d):
    p = os.path.join(d, "f")
    wr(p, b"keep")
    fd = os.open(p, os.O_RDWR)
    try:
        n = os.write(fd, b"")
        eq(n, 0, "zero-length write n")
    finally:
        os.close(fd)
    eq(rd(p), b"keep", "zero-length write must not change data")
    eq(os.path.getsize(p), 4, "zero-length write must not change size")


@test
def fcntl_setfl_oappend(d):
    p = os.path.join(d, "f")
    wr(p, b"aaa")
    fd = os.open(p, os.O_RDWR)
    try:
        fl = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_APPEND)
        os.lseek(fd, 0, os.SEEK_SET)
        os.write(fd, b"ZZZ")
    finally:
        os.close(fd)
    eq(rd(p), b"aaaZZZ", "F_SETFL O_APPEND writes at EOF")


@test
def two_fds_size_mtime(d):
    p = os.path.join(d, "f")
    fd1 = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    fd2 = os.open(p, os.O_RDONLY)
    try:
        os.write(fd1, b"abcdef")
        os.fsync(fd1)
        st = os.fstat(fd2)
        eq(st.st_size, 6, "other-fd fstat size")
        os.lseek(fd2, 0, os.SEEK_SET)
        eq(os.read(fd2, 6), b"abcdef", "other-fd read")
    finally:
        os.close(fd1)
        os.close(fd2)


@test
def trunc_open_other_fd(d):
    p = os.path.join(d, "f")
    wr(p, b"0123456789")
    fd1 = os.open(p, os.O_RDWR)
    fd2 = os.open(p, os.O_WRONLY | os.O_TRUNC)
    try:
        os.write(fd2, b"Z")
        os.fsync(fd2)
        eq(os.fstat(fd1).st_size, 1, "open fd sees O_TRUNC size")
        os.lseek(fd1, 0, os.SEEK_SET)
        eq(os.read(fd1, 8), b"Z", "open fd sees O_TRUNC data")
    finally:
        os.close(fd1)
        os.close(fd2)


@test
def link_across_dirs(d):
    a = os.path.join(d, "a")
    b = os.path.join(d, "b")
    os.mkdir(a)
    os.mkdir(b)
    wr(os.path.join(a, "f"), b"shared")
    os.link(os.path.join(a, "f"), os.path.join(b, "g"))
    eq(os.stat(os.path.join(a, "f")).st_nlink, 2, "nlink after cross-dir link")
    eq(os.stat(os.path.join(a, "f")).st_ino,
       os.stat(os.path.join(b, "g")).st_ino, "same ino")
    eq(rd(os.path.join(b, "g")), b"shared", "read via other dir")


@test
def last_link_unlink_other_dir(d):
    a = os.path.join(d, "a")
    b = os.path.join(d, "b")
    os.mkdir(a)
    os.mkdir(b)
    wr(os.path.join(a, "f"), b"keep")
    os.link(os.path.join(a, "f"), os.path.join(b, "g"))
    os.unlink(os.path.join(a, "f"))
    eq(rd(os.path.join(b, "g")), b"keep", "survived first-name unlink")
    eq(os.stat(os.path.join(b, "g")).st_nlink, 1, "nlink after first unlink")
    os.unlink(os.path.join(b, "g"))
    assert not os.path.exists(os.path.join(b, "g"))


@test
def nlink_after_unlink_open(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    os.write(fd, b"open-nlink")
    os.unlink(p)
    try:
        st = os.fstat(fd)
        eq(st.st_nlink, 0, "nlink 0 after last-name unlink")
        os.lseek(fd, 0, os.SEEK_SET)
        eq(os.read(fd, 10), b"open-nlink", "data after last unlink")
    finally:
        os.close(fd)
    assert not os.path.exists(p)


@test
def o_nofollow(d):
    wr(os.path.join(d, "t"), b"x")
    os.symlink("t", os.path.join(d, "l"))
    expect_err(errno.ELOOP, os.open, os.path.join(d, "l"),
               os.O_RDONLY | os.O_NOFOLLOW)


@test
def o_directory(d):
    wr(os.path.join(d, "f"), b"x")
    os.mkdir(os.path.join(d, "sub"))
    expect_err(errno.ENOTDIR, os.open, os.path.join(d, "f"),
               os.O_RDONLY | os.O_DIRECTORY)
    fd = os.open(os.path.join(d, "sub"), os.O_RDONLY | os.O_DIRECTORY)
    os.close(fd)


@test
def ctime_on_link_rename(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    c0 = os.lstat(p).st_ctime
    time.sleep(1.1)
    os.link(p, os.path.join(d, "g"))
    c1 = os.lstat(p).st_ctime
    if c1 < c0:
        raise Fail("ctime went backwards on link")
    if c1 == c0:
        raise Fail("ctime did not bump on link")
    time.sleep(1.1)
    os.rename(os.path.join(d, "g"), os.path.join(d, "h"))
    c2 = os.lstat(p).st_ctime
    if c2 < c1:
        raise Fail("ctime went backwards on rename of other name")
    if c2 == c1:
        raise Fail("ctime did not bump on rename of other name")


@test
def st_ino_unique_hardlink_same(d):
    a = os.path.join(d, "a")
    b = os.path.join(d, "b")
    wr(a, b"1")
    wr(b, b"2")
    ia, ib = os.stat(a).st_ino, os.stat(b).st_ino
    if ia == ib:
        raise Fail("two creates share ino %d" % ia)
    os.link(a, os.path.join(d, "c"))
    eq(os.stat(a).st_ino, os.stat(os.path.join(d, "c")).st_ino,
       "hardlink same ino")
    eq(os.stat(a).st_dev, os.stat(os.path.join(d, "c")).st_dev,
       "hardlink same dev")


@test
def create_excl_two_proc(d):
    p = os.path.join(d, "f")
    snippet = (
        "import os,sys,errno\n"
        "p=sys.argv[1]\n"
        "try:\n"
        "    fd=os.open(p, os.O_CREAT|os.O_EXCL|os.O_WRONLY, 0o644)\n"
        "    os.write(fd, b'x'); os.close(fd); sys.exit(0)\n"
        "except OSError as e:\n"
        "    sys.exit(17 if e.errno==errno.EEXIST else 2)\n"
    )
    procs = [
        subprocess.Popen([sys.executable, "-c", snippet, p])
        for _ in range(2)
    ]
    rcs = [pr.wait() for pr in procs]
    wins = rcs.count(0)
    exists = rcs.count(17)
    if wins != 1 or exists != 1:
        raise Fail("O_EXCL two-proc rcs=%s (want one 0 and one 17)" % rcs)
    eq(rd(p), b"x", "winner content")


@test
def concurrent_overlap_write(d):
    """Overlapping pwrites: size stays 4k and the file is one pattern, not a mix."""
    import threading
    p = os.path.join(d, "f")
    wr(p, b"\x00" * 4096)
    errors = []

    def worker(byte):
        try:
            fd = os.open(p, os.O_RDWR)
            os.pwrite(fd, byte * 4096, 0)
            os.close(fd)
        except Exception as e:  # noqa: BLE001
            errors.append(e)

    t0 = threading.Thread(target=worker, args=(b"A",))
    t1 = threading.Thread(target=worker, args=(b"B",))
    t0.start()
    t1.start()
    t0.join()
    t1.join()
    if errors:
        raise Fail("overlap write: %s" % errors[0])
    data = rd(p)
    eq(len(data), 4096, "overlap size")
    if data != b"A" * 4096 and data != b"B" * 4096:
        raise Fail("torn overlap write (mix of A and B)")


# ==========================================================================
# Extra POSIX corners (getattr size, hardlinks, rename, *at, holes)
# ==========================================================================
@test
def fstat_size_after_write(d):
    """Kernel i_size after a write must match fstat without a reopen."""
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        os.write(fd, b"abcdef")
        eq(os.fstat(fd).st_size, 6, "fstat size after write")
        os.lseek(fd, 0, os.SEEK_SET)
        eq(os.read(fd, 6), b"abcdef", "read after fstat")
    finally:
        os.close(fd)


@test
def trunc_shrink_same_fd(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        os.write(fd, b"0123456789")
        os.ftruncate(fd, 4)
        eq(os.fstat(fd).st_size, 4, "ftruncate size")
        os.lseek(fd, 0, os.SEEK_SET)
        eq(os.read(fd, 16), b"0123", "read after shrink")
    finally:
        os.close(fd)


@test
def unlink_open_then_recreate(d):
    """Unlinked fd keeps old bytes; a new name is a new inode."""
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    os.write(fd, b"old-bytes")
    ino0 = os.fstat(fd).st_ino
    os.unlink(p)
    wr(p, b"new")
    ino1 = os.stat(p).st_ino
    try:
        if ino0 == ino1:
            raise Fail("recreate reused inode %d of the unlinked fd" % ino0)
        os.lseek(fd, 0, os.SEEK_SET)
        eq(os.read(fd, 9), b"old-bytes", "unlinked fd still has old data")
        eq(rd(p), b"new", "new name is independent")
    finally:
        os.close(fd)


@test
def rename_to_self(d):
    p = os.path.join(d, "f")
    wr(p, b"same")
    os.rename(p, p)
    eq(rd(p), b"same", "rename-to-self")
    eq(os.stat(p).st_nlink, 1, "nlink after rename-to-self")


@test
def rename_empty_over_empty_dir(d):
    a = os.path.join(d, "a")
    b = os.path.join(d, "b")
    os.mkdir(a)
    os.mkdir(b)
    os.rename(a, b)
    assert os.path.isdir(b)
    assert not os.path.exists(a)
    eq(os.listdir(b), [], "replaced dir empty")


@test
def lstat_symlink_size(d):
    os.symlink("target-name", os.path.join(d, "l"))
    st = os.lstat(os.path.join(d, "l"))
    if not statmod.S_ISLNK(st.st_mode):
        raise Fail("lstat is not a symlink")
    eq(st.st_size, len("target-name"), "symlink st_size is target length")
    eq(os.readlink(os.path.join(d, "l")), "target-name", "readlink")


@test
def chmod_preserves_mtime(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.utime(p, ns=(1_000_000_000_000_000_000, 1_111_111_111_000_000_000))
    m0 = os.stat(p).st_mtime_ns
    os.chmod(p, 0o600)
    st = os.stat(p)
    eq(st.st_mtime_ns, m0, "chmod must not change mtime")
    eq(statmod.S_IMODE(st.st_mode), 0o600, "mode applied")


@test
def unlink_recreate_new_ino(d):
    """A still-open inode and its replacement must not share an inode number.

    Reuse after the last close is allowed by POSIX, so this test keeps the
    old descriptor open while the name is recreated.
    """
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        os.write(fd, b"a")
        i0 = os.fstat(fd).st_ino
        os.unlink(p)
        wr(p, b"b")
        i1 = os.stat(p).st_ino
        if i0 == i1:
            raise Fail("recreate reused inode %d while the old fd is open" % i0)
        eq(rd(p), b"b", "new content")
    finally:
        os.close(fd)


@test
def case_sensitive_names(d):
    wr(os.path.join(d, "Foo"), b"A")
    wr(os.path.join(d, "foo"), b"B")
    names = set(os.listdir(d))
    if "Foo" not in names or "foo" not in names:
        raise Fail("listdir missing Foo/foo: %s" % sorted(names))
    eq(rd(os.path.join(d, "Foo")), b"A", "Foo")
    eq(rd(os.path.join(d, "foo")), b"B", "foo")


@test
def o_append_pwrite_absolute(d):
    """Linux (unlike POSIX) makes pwrite honor O_APPEND and write at EOF."""
    p = os.path.join(d, "f")
    wr(p, b"aaaa")
    fd = os.open(p, os.O_RDWR | os.O_APPEND)
    try:
        n = os.pwrite(fd, b"ZZ", 1)
        eq(n, 2, "pwrite n")
    finally:
        os.close(fd)
    eq(rd(p), b"aaaaZZ", "Linux pwrite+O_APPEND appends")


@test
def two_fds_independent_offset(d):
    p = os.path.join(d, "f")
    wr(p, b"0123456789")
    a = os.open(p, os.O_RDONLY)
    b = os.open(p, os.O_RDONLY)
    try:
        eq(os.read(a, 3), b"012", "fd a")
        eq(os.read(b, 3), b"012", "fd b starts at 0")
        eq(os.read(a, 2), b"34", "fd a continues")
        eq(os.read(b, 2), b"34", "fd b continues independently")
    finally:
        os.close(a)
        os.close(b)


@test
def dir_nlink_after_rmdir_child(d):
    parent = os.path.join(d, "p")
    os.mkdir(parent)
    n0 = os.stat(parent).st_nlink
    os.mkdir(os.path.join(parent, "c"))
    if os.stat(parent).st_nlink != n0 + 1:
        raise Fail("mkdir child did not bump parent nlink (got %d want %d)" %
                   (os.stat(parent).st_nlink, n0 + 1))
    os.rmdir(os.path.join(parent, "c"))
    eq(os.stat(parent).st_nlink, n0, "rmdir child restores parent nlink")


@test
def access_f_ok_after_unlink(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.unlink(p)
    if os.access(p, os.F_OK):
        raise Fail("F_OK true after unlink")


@test
def write_hole_pread_zeros(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        os.pwrite(fd, b"Z", 100)
        eq(os.pread(fd, 5, 0), b"\x00" * 5, "leading hole is zeros")
        eq(os.pread(fd, 1, 100), b"Z", "data at 100")
        eq(os.fstat(fd).st_size, 101, "size after hole write")
    finally:
        os.close(fd)


@test
def hardlink_three_names(d):
    a = os.path.join(d, "a")
    wr(a, b"abc")
    os.link(a, os.path.join(d, "b"))
    os.link(a, os.path.join(d, "c"))
    eq(os.stat(a).st_nlink, 3, "nlink=3")
    with open(os.path.join(d, "b"), "wb") as f:
        f.write(b"XYZ")
    eq(rd(os.path.join(d, "c")), b"XYZ", "write via b seen via c")
    os.unlink(os.path.join(d, "b"))
    eq(os.stat(a).st_nlink, 2, "nlink=2 after one unlink")
    eq(rd(a), b"XYZ", "surviving names")


@test
def rename_file_over_symlink(d):
    wr(os.path.join(d, "t"), b"tgt")
    os.symlink("t", os.path.join(d, "l"))
    wr(os.path.join(d, "s"), b"src")
    os.rename(os.path.join(d, "s"), os.path.join(d, "l"))
    assert not os.path.islink(os.path.join(d, "l")), "dest is no longer a link"
    eq(rd(os.path.join(d, "l")), b"src", "rename-over-symlink content")
    eq(rd(os.path.join(d, "t")), b"tgt", "symlink target file survives")


@test
def flock_unlock_on_close(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    fd1 = os.open(p, os.O_RDWR)
    fcntl.flock(fd1, fcntl.LOCK_EX)
    os.close(fd1)
    fd2 = os.open(p, os.O_RDWR)
    try:
        fcntl.flock(fd2, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(fd2, fcntl.LOCK_UN)
    except OSError as e:
        raise Fail("LOCK_EX after other fd close: %s" % e)
    finally:
        os.close(fd2)


@test
def mkdirat_unlinkat(d):
    dirfd = os.open(d, os.O_RDONLY)
    try:
        os.mkdir("sub", dir_fd=dirfd)
        wr(os.path.join(d, "sub", "f"), b"x")
        expect_err(errno.ENOTEMPTY, os.rmdir, "sub", dir_fd=dirfd)
        os.unlink(os.path.join(d, "sub", "f"))
        os.rmdir("sub", dir_fd=dirfd)
    finally:
        os.close(dirfd)
    assert not os.path.exists(os.path.join(d, "sub"))


@test
def trailing_slash_on_file(d):
    wr(os.path.join(d, "f"), b"x")
    expect_err(errno.ENOTDIR, os.stat, os.path.join(d, "f") + "/")
    expect_err(errno.ENOTDIR, os.open, os.path.join(d, "f") + "/", os.O_RDONLY)


@test
def creat_existing_dir_eisdir(d):
    os.mkdir(os.path.join(d, "sub"))
    expect_err(errno.EISDIR, os.open, os.path.join(d, "sub"),
               os.O_CREAT | os.O_WRONLY, 0o644)


@test
def fsync_then_fstat_size(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        os.write(fd, b"Q" * 777)
        os.fsync(fd)
        eq(os.fstat(fd).st_size, 777, "fstat after fsync")
    finally:
        os.close(fd)
    eq(os.path.getsize(p), 777, "path size after fsync")


@test
@serial
def chdir_dotdot_after_mkdir(d):
    sub = os.path.join(d, "sub")
    os.mkdir(sub)
    wr(os.path.join(sub, "f"), b"in")
    cwd = os.getcwd()
    try:
        os.chdir(sub)
        eq(rd("f"), b"in", "relative in sub")
        os.chdir("..")
        assert os.path.isdir("sub"), "dotdot is the testdir"
    finally:
        os.chdir(cwd)


@test
def mtime_bumps_on_write(d):
    p = os.path.join(d, "f")
    wr(p, b"a")
    os.utime(p, ns=(1_000_000_000_000_000_000, 1_000_000_000_000_000_000))
    m0 = os.stat(p).st_mtime_ns
    time.sleep(0.02)
    with open(p, "ab") as f:
        f.write(b"b")
    m1 = os.stat(p).st_mtime_ns
    if m1 <= m0:
        raise Fail("mtime did not advance on write (%d -> %d)" % (m0, m1))


@test
def dir_times_bump_on_child_mutation(d):
    """A child create/rename/unlink must move the PARENT dir's mtime+ctime on
    the same client — a stale parent stat is the classic FUSE attr-cache bug
    (the 0j lookup memo must be invalidated by local mutations)."""
    sub = os.path.join(d, "watched")
    os.mkdir(sub)

    def times():
        st = os.stat(sub)
        return (st.st_mtime_ns, st.st_ctime_ns)

    t0 = times()
    _wait_dir_tick()
    wr(os.path.join(sub, "f"), b"x")
    t1 = times()
    if t1 == t0:
        raise Fail("dir times did not move on create")
    _wait_dir_tick()
    os.rename(os.path.join(sub, "f"), os.path.join(sub, "g"))
    t2 = times()
    if t2 == t1:
        raise Fail("dir times did not move on rename")
    _wait_dir_tick()
    os.unlink(os.path.join(sub, "g"))
    t3 = times()
    if t3 == t2:
        raise Fail("dir times did not move on unlink")


@test
def rename_readdir_lookup_consistent(d):
    """After a rename the same client's two views must agree: the old name is
    gone from BOTH lookup (stat) and readdir (listdir), the new name is in
    both. W56: on the export root the views diverged (lookup ghosted while
    readdir was already correct) — pin the nested case here."""
    # file
    wr(os.path.join(d, "fa"), b"x")
    os.rename(os.path.join(d, "fa"), os.path.join(d, "fb"))
    listed = os.listdir(d)
    if "fa" in listed:
        raise Fail("old file name still in readdir after rename")
    if "fb" not in listed:
        raise Fail("new file name missing from readdir after rename")
    expect_err(errno.ENOENT, os.stat, os.path.join(d, "fa"))
    eq(rd(os.path.join(d, "fb")), b"x", "file content at the new name")
    # directory (the W56 shape, but nested — root is @root territory)
    os.mkdir(os.path.join(d, "da"))
    os.rename(os.path.join(d, "da"), os.path.join(d, "db"))
    listed = os.listdir(d)
    if "da" in listed:
        raise Fail("old dir name still in readdir after rename")
    if "db" not in listed:
        raise Fail("new dir name missing from readdir after rename")
    expect_err(errno.ENOENT, os.stat, os.path.join(d, "da"))
    eq(os.path.isdir(os.path.join(d, "db")), True, "dir at the new name")


@test
def rename_dir_same_parent(d):
    os.mkdir(os.path.join(d, "old"))
    wr(os.path.join(d, "old", "f"), b"keep")
    os.rename(os.path.join(d, "old"), os.path.join(d, "new"))
    assert not os.path.exists(os.path.join(d, "old"))
    eq(rd(os.path.join(d, "new", "f")), b"keep", "dir rename keeps child")


@test
def link_of_symlink(d):
    """Linux link() does not follow: both names are the same symlink inode."""
    wr(os.path.join(d, "t"), b"data")
    os.symlink("t", os.path.join(d, "l"))
    os.link(os.path.join(d, "l"), os.path.join(d, "h"),
            follow_symlinks=False)
    if not os.path.islink(os.path.join(d, "h")):
        raise Fail("link() of a symlink followed the target")
    eq(os.lstat(os.path.join(d, "l")).st_ino,
       os.lstat(os.path.join(d, "h")).st_ino, "same symlink ino")
    eq(os.lstat(os.path.join(d, "l")).st_nlink, 2, "symlink nlink")
    eq(os.readlink(os.path.join(d, "h")), "t", "other name readlink")
    eq(rd(os.path.join(d, "h")), b"data", "follow via new name")


@test
def concurrent_extend_high_then_low(d):
    """High-offset write first, low-offset second: size must stay the high end."""
    import threading
    p = os.path.join(d, "f")
    wr(p, b"")
    errors = []

    def high():
        try:
            fd = os.open(p, os.O_RDWR)
            os.pwrite(fd, b"H", 8 << 20)
            os.close(fd)
        except Exception as e:  # noqa: BLE001
            errors.append(e)

    def low():
        time.sleep(0.05)
        try:
            fd = os.open(p, os.O_RDWR)
            os.pwrite(fd, b"L", 1 << 20)
            os.close(fd)
        except Exception as e:  # noqa: BLE001
            errors.append(e)

    t0 = threading.Thread(target=high)
    t1 = threading.Thread(target=low)
    t0.start()
    t1.start()
    t0.join()
    t1.join()
    if errors:
        raise Fail("extend: %s" % errors[0])
    eq(os.path.getsize(p), (8 << 20) + 1, "size stayed high end")
    fd = os.open(p, os.O_RDONLY)
    try:
        eq(os.pread(fd, 1, 8 << 20), b"H", "high byte")
        eq(os.pread(fd, 1, 1 << 20), b"L", "low byte")
    finally:
        os.close(fd)


@test
def mtime_monotonic_many_writes(d):
    """st_mtime_ns must not go backwards across many overwrites."""
    p = os.path.join(d, "f")
    wr(p, b"x")
    last = os.stat(p).st_mtime_ns
    for i in range(80):
        with open(p, "ab") as f:
            f.write(b"y")
        now = os.stat(p).st_mtime_ns
        if now < last:
            raise Fail("mtime went backwards at write %d (%s < %s)" %
                       (i, now, last))
        last = now


@test
def trunc_zero_then_high_pwrite(d):
    """truncate(0) then pwrite at 1 MiB: prefix is a hole, not stale bytes."""
    p = os.path.join(d, "f")
    wr(p, b"OLDPREFIX")
    os.truncate(p, 0)
    fd = os.open(p, os.O_RDWR)
    try:
        os.pwrite(fd, b"N", 1 << 20)
    finally:
        os.close(fd)
    eq(os.path.getsize(p), (1 << 20) + 1, "size")
    fd = os.open(p, os.O_RDONLY)
    try:
        eq(os.pread(fd, 9, 0), b"\x00" * 9, "no stale prefix")
        eq(os.pread(fd, 1, 1 << 20), b"N", "high byte")
    finally:
        os.close(fd)


@test
def write_beyond_eof_then_seek_end(d):
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        os.pwrite(fd, b"X", 50)
        end = os.lseek(fd, 0, os.SEEK_END)
        eq(end, 51, "SEEK_END after pwrite past EOF")
    finally:
        os.close(fd)


# Two-client live visibility is posix_2client.py (run_tests.sh posix2).
#
# ==========================================================================
# Runner
# ==========================================================================
def invoke(fn, tdir):
    os.makedirs(tdir, exist_ok=True)
    try:
        fn(tdir)
    except Fail as e:
        if getattr(e, "soft", False):
            return "SKIP", str(e)
        return "FAIL", str(e)
    except TestTimeout as e:
        return "FAIL", str(e)
    except Exception as e:  # noqa: BLE001
        try:
            return "FAIL", "%s: %s" % (type(e).__name__, e)
        except TestTimeout:
            return "FAIL", "%s: %s" % (type(e).__name__, e)
    return "PASS", ""


def format_result_line(name, status, detail):
    """One TSV row. Tabs in the detail stay in the third field."""
    return "%s\t%s\t%s\n" % (name, status, (detail or "").replace("\n", " "))


def _write_status(path, status, detail):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write("%s\t%s\n" % (status, (detail or "").replace("\n", " ")))
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def _read_status_file(path):
    try:
        with open(path) as f:
            line = f.readline().rstrip("\n")
    except OSError:
        return None
    if "\t" not in line:
        return None
    status, detail = line.split("\t", 1)
    if status not in ("PASS", "FAIL", "SKIP"):
        return None
    return status, detail


def _unlink_quiet(path):
    try:
        os.unlink(path)
    except OSError:
        pass


# Test hooks. None uses the real process operations. The deadline
# self-test installs these so a synthetic process-group id is never
# passed to killpg, waitpid, or a group-existence probe.
_signal_group_hook = None
_reap_hook = None
_pgid_alive_hook = None


def _signal_group(pgid, sig):
    """Deliver sig to a process group. The test hook replaces os.killpg."""
    hook = _signal_group_hook
    if hook is not None:
        hook(pgid, sig)
        return
    os.killpg(pgid, sig)


def _reap_pid(pid):
    """Collect a direct child so a zombie does not keep its process group visible."""
    if not pid or pid <= 0:
        return
    hook = _reap_hook
    if hook is not None:
        hook(pid)
        return
    try:
        os.waitpid(pid, os.WNOHANG)
    except OSError:
        pass


def _kill_pgid(pgid, grace=0.5):
    """SIGKILL a process group and wait until it is empty.

    The leader is reaped. A killed direct child stays in the group as a
    zombie until wait/poll, and that used to look like a survivor for the
    whole grace period.
    """
    if not pgid or pgid <= 0:
        return True
    try:
        _signal_group(pgid, signal.SIGKILL)
    except OSError:
        pass
    _reap_pid(pgid)
    if not _pgid_alive(pgid):
        return True
    deadline = time.monotonic() + grace
    while time.monotonic() < deadline:
        _reap_pid(pgid)
        if not _pgid_alive(pgid):
            return True
        time.sleep(0.02)
    _reap_pid(pgid)
    return not _pgid_alive(pgid)


def _stop_registered(procs, budget=1.0):
    """SIGKILL every group first, then reap under one shared deadline.

    Returns pids whose groups are still occupied. An unreaped zombie is not
    a survivor. Waiting per group would blow the outer 5s SIGTERM-to-SIGKILL
    budget once more than a handful of workers are live.
    """
    items = []
    for proc in procs:
        if hasattr(proc, "poll"):
            items.append(proc)
        elif proc:
            items.append(proc)
    for proc in items:
        pid = proc.pid if hasattr(proc, "pid") else int(proc)
        if pid and pid > 0:
            try:
                _signal_group(pid, signal.SIGKILL)
            except OSError:
                pass

    def reap():
        for proc in items:
            if hasattr(proc, "poll"):
                try:
                    proc.poll()
                except Exception:
                    pass
            pid = proc.pid if hasattr(proc, "pid") else int(proc)
            _reap_pid(pid)

    def survivors():
        remaining = deadline - time.monotonic()
        # Do not start a process-table read after the budget is gone.
        # The previous call's answer stands.
        if remaining <= 0:
            return last["rows"]
        snapshot = _process_snapshot(remaining)
        out = []
        for proc in items:
            pid = proc.pid if hasattr(proc, "pid") else int(proc)
            if not pid:
                continue
            # Pass the snapshot through even when it is None. None means
            # "this read failed"; it must not be fetched again per worker.
            running = _running_in_group(pid, snapshot)
            if running is None:
                if _pgid_alive(pid):
                    out.append(pid)
            elif running:
                out.append(pid)
        last["rows"] = out
        return out

    deadline = time.monotonic() + budget
    last = {"rows": []}
    reap()
    while time.monotonic() < deadline:
        left = survivors()
        if not left:
            return []
        reap()
        if time.monotonic() >= deadline:
            return left
        time.sleep(min(0.02, deadline - time.monotonic()))
        reap()
    return last["rows"]


def _pgid_alive(pgid):
    """True while any process still belongs to this process group.

    The leader exiting does not empty the group: a child that ignored
    SIGTERM, or one blocked in a filesystem call, keeps the same pgid.
    A zombie still belongs to the group until it is collected, so this is
    the wrong test for "a worker is still running."
    """
    if not pgid or pgid <= 0:
        return False
    hook = _pgid_alive_hook
    if hook is not None:
        return hook(pgid)
    try:
        os.killpg(pgid, 0)
    except OSError as e:
        if e.errno == errno.ESRCH:
            return False
        # EPERM means something in the group exists and we cannot signal it.
        return True
    return True


# Sentinel: the caller did not supply a snapshot, so read the process
# table. None is a different value and means that read already failed.
_SNAPSHOT_OMITTED = object()

# Test hook. None uses ps. A replacement is called as hook(timeout).
_ps_hook = None


def _process_snapshot(timeout):
    """One process table read: (pid, pgid, state), or None if it failed.

    The lookup is limited to `timeout` seconds. None is reused by the
    caller; it must not be read as "please try ps again."
    """
    if timeout <= 0:
        return None
    hook = _ps_hook
    try:
        if hook is not None:
            out = hook(timeout)
        else:
            proc = subprocess.run(
                ["ps", "-ax", "-o", "pid=,pgid=,state="],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                timeout=timeout)
            if proc.returncode != 0:
                return None
            out = proc.stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    if out is None:
        return None
    rows = []
    for line in out.decode("ascii", "replace").splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            pid = int(parts[0])
            pgrp = int(parts[1])
        except ValueError:
            continue
        rows.append((pid, pgrp, parts[2]))
    return rows


def _running_in_group(pgid, snapshot=_SNAPSHOT_OMITTED):
    """Members of pgid that are not zombies.

    SIGKILL turns a grandchild into a zombie reparented to init. That
    zombie still answers killpg(pgid, 0) until init collects it, so a
    group of only zombies is not a worker that is still running.
    Returns None when the process list cannot be read. A None snapshot
    is that failure and is not fetched again.
    """
    if not pgid or pgid <= 0:
        return []
    if snapshot is _SNAPSHOT_OMITTED:
        snapshot = _process_snapshot(1.0)
    if snapshot is None:
        return None
    return [pid for pid, pgrp, state in snapshot
            if pgrp == pgid and not state.startswith("Z")]


def _kill_group(proc, grace=1.0):
    """Stop the leader and every same-group descendant.

    True only when the process group is empty. The leader's exit status is
    not that proof: a descendant can survive SIGTERM after the leader has
    already been reaped. False means at least one member is still alive.
    """
    pgid = proc.pid
    proc.poll()

    def settled():
        proc.poll()
        return not _pgid_alive(pgid)

    if settled():
        return True
    try:
        _signal_group(pgid, signal.SIGTERM)
    except OSError:
        pass
    deadline = time.monotonic() + grace
    while time.monotonic() < deadline:
        if settled():
            return True
        time.sleep(0.02)
    try:
        _signal_group(pgid, signal.SIGKILL)
    except OSError:
        pass
    deadline = time.monotonic() + grace
    while time.monotonic() < deadline:
        if settled():
            return True
        time.sleep(0.02)
    return settled()


def _classify_worker(rc, got):
    """Accept PASS/SKIP only from a process that exited 0.

    `got` is (status, detail) or None. A published PASS followed by a
    nonzero exit or a signal is a failure; the published text stays in
    the detail.
    """
    if rc is None:
        return "FAIL", "worker has no exit status"
    published = ""
    if got is not None:
        published = " after publishing %s" % got[0]
        if got[1]:
            published += ": " + got[1]
    if rc < 0:
        return "FAIL", "worker killed by signal %s%s" % (-rc, published)
    if got is None:
        return "FAIL", "worker exited %s without a result" % rc
    status, detail = got
    if rc != 0 and status in ("PASS", "SKIP"):
        return "FAIL", "worker exited %s after publishing %s: %s" % (
            rc, status, detail)
    return status, detail


def _finish_worker(proc, result, kill_group):
    """Reap the whole group, then classify the leader's exit and result file.

    Returns (status, detail, group_dead). The group stays the caller's
    problem when group_dead is false.
    """
    dead = kill_group(proc)
    rc = proc.poll()
    got = _read_status_file(result)
    if not dead:
        status, detail = _classify_worker(rc, got)
        return ("FAIL",
                "worker group still alive (" + detail + ")",
                False)
    status, detail = _classify_worker(rc, got)
    return status, detail, True


class _SpawnGate(object):
    """Defer cancellation until the new process group is in `live`.

    A signal between Popen and registration used to exit the parent while
    the child, already in its own session, kept running.
    """

    def __init__(self):
        self.defer = 0
        self.pending = None


def _run_pool(items, jobs, spawn, on_done, kill_group=None, live=None,
              gate=None, on_cancel=None):
    """Run items of (name, timeout_s, payload) in up to `jobs` processes.

    `on_done(name, status, detail)` returns true to stop starting new work.
    A timeout kills the process group. If the process survives SIGKILL, that
    name is returned and no further item is started. The unstarted tail is
    the second return value. timeout_s <= 0 disables the budget.
    """
    if kill_group is None:
        kill_group = _kill_group
    queue = list(items)
    inflight = []
    stop = False
    stuck = None

    def drop_live(proc):
        if live is None:
            return
        live[:] = [p for p in live if getattr(p, "pid", p) != proc.pid]

    def note_live(proc):
        if live is None:
            return
        if all(getattr(p, "pid", p) != proc.pid for p in live):
            live.append(proc)

    while (queue or inflight) and stuck is None:
        while queue and len(inflight) < max(1, jobs) and not stop:
            name, timeout, payload = queue.pop(0)
            pending = None
            raised = None
            if gate is not None:
                gate.defer += 1
            try:
                proc, result = spawn(name, payload)
                note_live(proc)
            except BaseException as exc:
                raised = exc
            finally:
                if gate is not None:
                    gate.defer -= 1
                    if gate.defer == 0:
                        pending = gate.pending
                        gate.pending = None
            # The child is registered before a deferred signal is allowed
            # to exit. on_cancel does not return.
            if pending is not None and on_cancel is not None:
                on_cancel(pending, None)
            if raised is not None:
                raise raised
            inflight.append((name, proc, result, time.monotonic(), timeout))
        if not inflight:
            break
        time.sleep(0.05)
        now = time.monotonic()
        still = []
        for name, proc, result, start, timeout in inflight:
            if stuck is not None:
                still.append((name, proc, result, start, timeout))
                continue
            rc = proc.poll()
            if rc is not None:
                # The leader is gone. Descendants in its process group are
                # not. Leave the pgid in `live` until the group is empty.
                st, det, dead = _finish_worker(proc, result, kill_group)
                if not dead:
                    on_done(name, st, det)
                    stuck = name
                    continue
                drop_live(proc)
                _unlink_quiet(result)
                if on_done(name, st, det):
                    stop = True
                continue
            if timeout > 0 and now - start >= timeout:
                st, det, dead = _finish_worker(proc, result, kill_group)
                if not dead:
                    on_done(name, "FAIL",
                            "timeout after %ss; worker group still alive" %
                            timeout)
                    stuck = name
                    continue
                drop_live(proc)
                _unlink_quiet(result)
                if on_done(name, "FAIL", "timeout after %ss" % timeout):
                    stop = True
                continue
            still.append((name, proc, result, start, timeout))
        inflight = still
    if stuck is not None:
        # Stop siblings. A group that survives stays in `live` so a signal
        # handler can still see it, and no later test is started.
        for name, proc, result, start, timeout in inflight:
            st, det, dead = _finish_worker(proc, result, kill_group)
            if not dead:
                on_done(name, "FAIL",
                        "worker group still alive after %s" % stuck)
                continue
            drop_live(proc)
            _unlink_quiet(result)
            on_done(name, st, det)
    return stuck, queue


def _posix_hold(name):
    """Test hook. Sleep in this worker, with a child that ignores SIGTERM."""
    if os.environ.get("EFS_POSIX_HOLD") != "start":
        return
    match = os.environ.get("EFS_POSIX_HOLD_MATCH")
    if match and match not in name:
        return
    seconds = float(os.environ.get("EFS_POSIX_HOLD_S", "30"))
    pidfile = os.environ.get("EFS_POSIX_HOLD_PID")
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


def _worker_main(name, tdir, result_path):
    _posix_hold(name)
    try:
        fns = dict(TESTS)
        if name not in fns:
            _write_status(result_path, "FAIL", "unknown test %s" % name)
            os._exit(1)
        st, det = invoke(fns[name], tdir)
        _write_status(result_path, st, det)
    except Exception as e:  # noqa: BLE001
        try:
            _write_status(result_path, "FAIL",
                          "%s: %s" % (type(e).__name__, e))
        except Exception:
            pass
        os._exit(1)
    os._exit(0)


def _parse_cli(args):
    """Parse suite argv. Mount and results paths are absolute at parse time,
    before any chdir, so a relative mount survives the later cwd change."""
    if not args or args[0].startswith("-"):
        return None
    mnt = os.path.abspath(args[0])
    results_file = None
    keep = False
    stop = False
    filt = None
    tag = None
    test_timeout = int(os.environ.get("POSIX_TEST_SEC", "15"))
    jobs = int(os.environ.get("POSIX_JOBS", "16"))
    i = 1
    while i < len(args):
        if args[i] == "--results":
            results_file = os.path.abspath(args[i + 1])
            i += 2
        elif args[i] == "--keep":
            keep = True
            i += 1
        elif args[i] == "--stop":
            stop = True
            i += 1
        elif args[i] == "--filter":
            filt = args[i + 1]
            i += 2
        elif args[i] == "--timeout-s":
            test_timeout = int(args[i + 1])
            i += 2
        elif args[i] == "--jobs":
            jobs = int(args[i + 1])
            i += 2
        elif args[i] == "--tag":
            tag = args[i + 1]
            i += 2
        else:
            i += 1
    return (mnt, results_file, keep, stop, filt, tag, test_timeout, jobs)


def _self_test():
    """Harness regressions that do not need a filesystem mount."""
    fails = []

    def expect(cond, msg):
        if not cond:
            fails.append(msg)

    td = tempfile.mkdtemp(prefix="posix-self-")
    old = os.getcwd()
    try:
        os.chdir(td)
        os.mkdir("mnt")
        parsed = _parse_cli(["mnt", "--results", "out.tsv"])
        os.chdir("/")
        expect(parsed[0] == os.path.join(td, "mnt"),
               "relative mount was not captured before chdir")
        expect(parsed[1] == os.path.join(td, "out.tsv"),
               "relative results path was not captured before chdir")
        expect(os.path.isdir(parsed[0]), "abspath mount is not a directory")
    finally:
        os.chdir(old)

    real_euid = os.geteuid
    os.geteuid = lambda: 0
    try:
        try:
            perm_file_000_denied(td)
        except Fail as e:
            expect(getattr(e, "soft", False),
                   "root permission skip was not soft: %s" % e)
        else:
            fails.append("root permission test returned as PASS")
    finally:
        os.geteuid = real_euid

    if hasattr(os, "setxattr"):
        real_setxattr = os.setxattr
        try:
            def denied(*_a, **_k):
                raise OSError(errno.EACCES, "injected")
            os.setxattr = denied
            try:
                opt_xattr(td)
            except Fail as e:
                expect(not getattr(e, "soft", False),
                       "xattr EACCES was treated as unsupported")
            else:
                fails.append("xattr EACCES returned as PASS")

            def unsupported(*_a, **_k):
                raise OSError(errno.EOPNOTSUPP, "injected")
            os.setxattr = unsupported
            try:
                opt_xattr(td)
            except Fail as e:
                expect(getattr(e, "soft", False),
                       "xattr EOPNOTSUPP was a hard failure")
            else:
                fails.append("xattr EOPNOTSUPP returned as PASS")
        finally:
            os.setxattr = real_setxattr

    import unicodedata
    nfc, nfd = _nfc_nfd_names()
    folded = {}
    folded[unicodedata.normalize("NFC", nfc)] = b"nfc"
    folded[unicodedata.normalize("NFC", nfd)] = b"nfd"
    expect(len(folded) == 1,
           "NFC/NFD pair does not collide under a normalizing adapter")

    # Exhaust the workers with blocked processes. The queued third item must
    # still run, and the whole pool must return well under the sleep time.
    outcomes = []
    procs = []

    def spawn_cmd(name, argv):
        proc = subprocess.Popen(argv, start_new_session=True,
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        procs.append(proc)
        return proc, os.path.join(td, name + ".noresult")

    def on_done(name, status, detail):
        outcomes.append((name, status))
        return False

    sleep30 = [sys.executable, "-c", "import time; time.sleep(30)"]
    short = [sys.executable, "-c", "import time; time.sleep(0.05)"]
    t0 = time.monotonic()
    stuck, left = _run_pool(
        [("a", 0.4, sleep30), ("b", 0.4, sleep30), ("c", 2.0, short)],
        2, spawn_cmd, on_done)
    elapsed = time.monotonic() - t0
    expect(stuck is None, "killable workers were reported stuck")
    expect(elapsed < 4.0, "pool ran for %.1fs (workers were not killed)" % elapsed)
    expect(left == [], "queued work was abandoned: %s" % [n for n, _t, _p in left])
    names = [n for n, _s in outcomes]
    expect(names.count("c") == 1, "queued item did not run: %s" % names)
    expect(all(p.poll() is not None for p in procs),
           "a timed-out worker was still alive after the pool returned")

    # A worker that ignores SIGTERM must still die on SIGKILL.
    ignore = [sys.executable, "-c",
              "import signal, time; signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(30)"]
    outcomes2 = []
    stuck, _left = _run_pool(
        [("ign", 0.3, ignore)], 1, spawn_cmd,
        lambda n, s, d: outcomes2.append(s) or False)
    expect(stuck is None, "SIGTERM-ignoring worker survived SIGKILL")
    expect(outcomes2 == ["FAIL"], "ignore-SIGTERM outcome %s" % outcomes2)

    # If the worker cannot be reaped, do not start queued work or a serial phase.
    serial = []
    started = []

    def spawn_track(name, argv):
        started.append(name)
        return spawn_cmd(name, argv)

    def kill_fail(proc):
        return False

    stuck, left = _run_pool(
        [("s1", 0.2, sleep30), ("s2", 0.2, sleep30), ("s3", 5.0, short)],
        2, spawn_track, lambda n, s, d: False, kill_group=kill_fail)
    if stuck is None:
        serial.append("ran")
    expect(stuck == "s1" or stuck == "s2", "unkillable worker was not stuck: %s" % stuck)
    expect(serial == [], "serial phase ran while a worker was alive")
    expect("s3" not in started, "queued work started after a stuck worker")
    for p in procs:
        if p.poll() is None:
            _kill_group(p)

    # Leader dies on SIGTERM; a same-group child ignores SIGTERM and keeps
    # writing. Cleanup is finished only when that child is gone.
    term_child = (
        "import os, signal, sys, time\n"
        "marker, pids = sys.argv[1], sys.argv[2]\n"
        "child = os.fork()\n"
        "if child == 0:\n"
        "    signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
        "    while True:\n"
        "        f = open(marker, 'a')\n"
        "        f.write('x')\n"
        "        f.close()\n"
        "        time.sleep(0.05)\n"
        "else:\n"
        "    f = open(pids, 'w')\n"
        "    f.write('%d\\n' % child)\n"
        "    f.close()\n"
        "    while True:\n"
        "        time.sleep(0.05)\n"
    )
    marker = os.path.join(td, "term-marker")
    pids_path = os.path.join(td, "term-pids")
    leader = subprocess.Popen(
        [sys.executable, "-c", term_child, marker, pids_path],
        start_new_session=True)
    try:
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline and not os.path.exists(pids_path):
            time.sleep(0.02)
        expect(os.path.exists(pids_path), "TERM child did not start")
        child_pid = int(open(pids_path).read().strip())
        expect(_kill_group(leader) is True, "_kill_group left the group alive")
        try:
            os.kill(child_pid, 0)
            fails.append("SIGTERM-ignoring descendant still alive")
        except OSError as e:
            expect(e.errno == errno.ESRCH, "kill child: %s" % e)
        size = os.path.getsize(marker) if os.path.exists(marker) else 0
        time.sleep(0.25)
        size2 = os.path.getsize(marker) if os.path.exists(marker) else 0
        expect(size == size2, "descendant wrote after the group was reaped")
    finally:
        _kill_pgid(leader.pid)

    # Normal leader exit while a nested child keeps mutating.
    tree = (
        "import os, signal, sys, time\n"
        "marker, result, pids = sys.argv[1:4]\n"
        "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
        "mid = os.fork()\n"
        "if mid == 0:\n"
        "    grand = os.fork()\n"
        "    if grand == 0:\n"
        "        while True:\n"
        "            f = open(marker, 'a')\n"
        "            f.write('y')\n"
        "            f.close()\n"
        "            time.sleep(0.05)\n"
        "    else:\n"
        "        while True:\n"
        "            time.sleep(0.2)\n"
        "else:\n"
        "    f = open(pids, 'w')\n"
        "    f.write('%d\\n' % mid)\n"
        "    f.close()\n"
        "    f = open(result, 'w')\n"
        "    f.write('PASS\\tdone\\n')\n"
        "    f.close()\n"
        "    os._exit(0)\n"
    )
    tree_marker = os.path.join(td, "tree-marker")
    tree_pids = os.path.join(td, "tree-pids")
    tree_out = {}

    def spawn_tree(name, script):
        result = os.path.join(td, name + ".res")
        proc = subprocess.Popen(
            [sys.executable, "-c", script, tree_marker, result, tree_pids],
            start_new_session=True)
        procs.append(proc)
        return proc, result

    stuck, _left = _run_pool(
        [("tree", 5.0, tree)], 1, spawn_tree,
        lambda n, s, d: tree_out.update({n: (s, d)}) or False)
    expect(stuck is None, "exited leader with children was stuck: %s" % stuck)
    expect(tree_out.get("tree", ("", ""))[0] == "PASS",
           "tree result %s" % (tree_out,))
    if os.path.exists(tree_pids):
        mid = int(open(tree_pids).read().strip())
        try:
            os.kill(mid, 0)
            fails.append("nested child still alive after the pool returned")
        except OSError as e:
            expect(e.errno == errno.ESRCH, "kill nested: %s" % e)
    else:
        fails.append("nested child pid file missing")
    grown = os.path.getsize(tree_marker) if os.path.exists(tree_marker) else 0
    time.sleep(0.25)
    grown2 = os.path.getsize(tree_marker) if os.path.exists(tree_marker) else 0
    expect(grown == grown2, "nested child wrote after the test was finished")

    # PASS published, then a nonzero exit, must not be accepted.
    exits = {}

    def spawn_exit(name, script):
        result = os.path.join(td, name + ".res")
        proc = subprocess.Popen(
            [sys.executable, "-c", script, result],
            start_new_session=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(proc)
        return proc, result

    exit_scripts = {
        "badexit": "import sys\nopen(sys.argv[1],'w').write('PASS\\t\\n')\nraise SystemExit(7)\n",
        "sig": "import os, signal, sys\nopen(sys.argv[1],'w').write('PASS\\t\\n')\nos.kill(os.getpid(), signal.SIGTERM)\n",
        "missing": "raise SystemExit(0)\n",
        "ok": "import sys\nopen(sys.argv[1],'w').write('PASS\\tok\\n')\nraise SystemExit(0)\n",
    }
    stuck, _left = _run_pool(
        [(n, 3.0, exit_scripts[n]) for n in ("badexit", "sig", "missing", "ok")],
        4, spawn_exit,
        lambda n, s, d: exits.update({n: (s, d)}) or False)
    expect(stuck is None, "exit-code pool stuck")
    expect(exits.get("badexit", ("",))[0] == "FAIL",
           "PASS+rc7 accepted: %r" % (exits.get("badexit"),))
    expect(exits.get("sig", ("",))[0] == "FAIL",
           "PASS+signal accepted: %r" % (exits.get("sig"),))
    expect(exits.get("missing", ("",))[0] == "FAIL",
           "missing result+rc0 accepted: %r" % (exits.get("missing"),))
    expect(exits.get("ok") == ("PASS", "ok"),
           "rc0 PASS rejected: %r" % (exits.get("ok"),))

    lockdir = os.path.join(td, "locks")
    os.mkdir(lockdir)
    try:
        flock_two_proc_exclusive(lockdir)
        fcntl_byte_range_lock(lockdir)
    except Fail as e:
        fails.append("lock oracle: %s" % e)

    try:
        _check_append_log(b"CORRUPT\n" * 200, list(range(4)), 50)
    except Fail:
        pass
    else:
        fails.append("corrupt append payload was accepted")
    try:
        _check_append_log(b"W0S0000\n" * 200, list(range(4)), 50)
    except Fail:
        pass
    else:
        fails.append("duplicated append records were accepted")
    try:
        _check_append_log(b"W0S0000\nW0S0001", [0], 2)
    except Fail:
        pass
    else:
        fails.append("torn append record was accepted")
    real_write = os.write
    os.write = lambda fd, data: 1
    try:
        try:
            _write_full(1, b"W0S0000\n")
        except Fail:
            pass
        else:
            fails.append("short append write was accepted")
    finally:
        os.write = real_write
    try:
        raise Fail("append worker: injected")
    except Fail:
        pass

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

    def _run_cli(mnt, extra_env, args, sig=None, wait_pid=None):
        env = os.environ.copy()
        env.update(extra_env)
        proc = subprocess.Popen(
            [sys.executable, os.path.abspath(__file__), mnt] + args,
            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True)
        try:
            if wait_pid:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and not os.path.exists(wait_pid):
                    if proc.poll() is not None:
                        break
                    time.sleep(0.02)
            if sig is not None and proc.poll() is None:
                os.kill(proc.pid, sig)
            try:
                rc = proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                _kill_pgid(proc.pid)
                rc = proc.wait(timeout=2)
            time.sleep(0.2)
            return rc
        finally:
            if proc.poll() is None:
                _kill_pgid(proc.pid)

    mnt = tempfile.mkdtemp(prefix="posix-cli-")
    try:
        hold_pid = os.path.join(td, "hold.pids")
        rc = _run_cli(
            mnt,
            {"EFS_POSIX_HOLD": "start", "EFS_POSIX_HOLD_S": "30",
             "EFS_POSIX_HOLD_PID": hold_pid},
            ["--filter", "basic_empty_file", "--timeout-s", "20", "--jobs", "1",
             "--results", os.path.join(td, "int.tsv")],
            sig=signal.SIGINT, wait_pid=hold_pid)
        if rc == 0:
            fails.append("SIGINT run exited 0")
        if _pids_alive(hold_pid):
            fails.append("SIGINT left workers %s" % _pids_alive(hold_pid))
        left = [n for n in os.listdir(mnt) if n.startswith("posix-")]
        if not left:
            fails.append("SIGINT deleted the test tree while workers were live")

        spawn_pid = os.path.join(td, "spawn.pid")
        spawn_tsv = os.path.join(td, "spawn.tsv")
        rc = _run_cli(
            mnt,
            {"EFS_POSIX_CANCEL_AT_SPAWN": "1", "EFS_POSIX_SPAWN_PID": spawn_pid},
            ["--filter", "basic_empty_file", "--timeout-s", "20", "--jobs", "1",
             "--results", spawn_tsv])
        if rc != 128 + signal.SIGTERM:
            fails.append("cancel-at-spawn rc=%s want signal exit" % rc)
        if not os.path.exists(spawn_pid):
            fails.append("cancel-at-spawn did not reach Popen")
        elif _pids_alive(spawn_pid):
            fails.append("cancel-at-spawn left %s" % _pids_alive(spawn_pid))
        spawn_text = open(spawn_tsv).read() if os.path.isfile(spawn_tsv) else ""
        if "TypeError" in spawn_text or "runner exception" in spawn_text:
            fails.append("cancel-at-spawn took the exception path")
        if "signal" not in spawn_text:
            fails.append("cancel-at-spawn missing signal NOTRUN")

        fake_tsv = os.path.join(td, "fake-surv.tsv")
        fake_pid = os.path.join(td, "fake-surv.pids")
        rc = _run_cli(
            mnt,
            {"EFS_POSIX_HOLD": "start", "EFS_POSIX_HOLD_S": "30",
             "EFS_POSIX_HOLD_PID": fake_pid,
             "EFS_POSIX_FAKE_SURVIVOR": "424242"},
            ["--filter", "basic_empty_file", "--timeout-s", "20", "--jobs", "1",
             "--results", fake_tsv],
            sig=signal.SIGTERM, wait_pid=fake_pid)
        fake_text = open(fake_tsv).read() if os.path.isfile(fake_tsv) else ""
        if rc != 128 + signal.SIGTERM:
            fails.append("fake survivor rc=%s" % rc)
        if not fake_text:
            fails.append("fake survivor wrote no results file")
        elif "424242" not in fake_text or "groups still alive" not in fake_text:
            fails.append("fake survivor diagnostic missing: %s" % fake_text[-240:])
        if "# complete" in fake_text:
            fails.append("fake survivor run marked complete")
        if not [n for n in os.listdir(mnt) if n.startswith("posix-")]:
            fails.append("fake survivor cleanup deleted the test tree")
        for pid in _pids_alive(fake_pid):
            _kill_pgid(int(pid))

        hold2 = os.path.join(td, "hold2.pids")
        rc = _run_cli(
            mnt,
            {"EFS_POSIX_HOLD": "start", "EFS_POSIX_HOLD_S": "30",
             "EFS_POSIX_HOLD_PID": hold2, "EFS_POSIX_SPAWN_PID": hold2,
             "EFS_POSIX_SPAWN_FAIL_AFTER": "1"},
            ["--filter", "basic_", "--timeout-s", "20", "--jobs", "2"])
        if rc == 0:
            fails.append("spawn failure exited 0")
        if _pids_alive(hold2):
            fails.append("spawn failure left workers %s" % _pids_alive(hold2))

        hold3 = os.path.join(td, "hold3.pids")
        rc = _run_cli(
            mnt,
            {"EFS_POSIX_HOLD": "start", "EFS_POSIX_HOLD_S": "30",
             "EFS_POSIX_HOLD_MATCH": "empty", "EFS_POSIX_HOLD_PID": hold3,
             "EFS_POSIX_TSV_FAIL": "1"},
            ["--filter", "basic_", "--timeout-s", "20", "--jobs", "2",
             "--results", os.path.join(td, "tsv.tsv")])
        if rc == 0:
            fails.append("tsv failure exited 0")
        if _pids_alive(hold3):
            fails.append("tsv failure left workers %s" % _pids_alive(hold3))

        rc = _run_cli(mnt, {}, ["--filter", "definitely_missing_test"])
        if rc != 2:
            fails.append("unmatched filter rc=%s" % rc)

        sleeper = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            start_new_session=True)
        t_kill = time.monotonic()
        killed = _kill_pgid(sleeper.pid, grace=0.5)
        kill_dt = time.monotonic() - t_kill
        if not killed or kill_dt >= 0.4 or sleeper.poll() is None:
            fails.append("zombie leader looked alive (ok=%s dt=%.3f poll=%s)" %
                         (killed, kill_dt, sleeper.poll()))
            _kill_pgid(sleeper.pid)

        # Leader exits and leaves a grandchild in its group. After SIGKILL
        # that grandchild is a zombie until init collects it. killpg still
        # sees the group; it is not a running worker, and cleanup must not
        # wait out the budget or skip the results file for it.
        orphan = (
            "import os, signal, time\n"
            "child = os.fork()\n"
            "if child == 0:\n"
            "    signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
            "    time.sleep(60)\n"
            "    os._exit(0)\n"
            "os._exit(0)\n"
        )
        leader = subprocess.Popen(
            [sys.executable, "-c", orphan], start_new_session=True)
        try:
            leader.wait(timeout=5)
        except subprocess.TimeoutExpired:
            fails.append("orphan leader did not exit")
            _kill_pgid(leader.pid)
        else:
            if not _pgid_alive(leader.pid):
                fails.append("grandchild was not left in the leader group")
            else:
                t_stop = time.monotonic()
                left = _stop_registered([leader], budget=1.0)
                stop_dt = time.monotonic() - t_stop
                if left:
                    fails.append("zombie grandchild reported as running: %s" % left)
                    for pid in left:
                        _kill_pgid(pid)
                if stop_dt >= 0.8:
                    fails.append("zombie grandchild burned the cleanup budget "
                                 "(%.3fs)" % stop_dt)

        class _Pid(object):
            def __init__(self, pid):
                self.pid = pid

            def poll(self):
                return None

        calls = {"n": 0}
        signaled = []
        reaped = []
        alive_checks = []
        synthetic = [1000000 + i for i in range(16)]

        def slow_ps(timeout):
            calls["n"] += 1
            time.sleep(min(0.16, max(0.0, timeout)))
            raise subprocess.TimeoutExpired(cmd="ps", timeout=timeout)

        def fake_signal(pgid, sig):
            signaled.append((pgid, sig))

        def fake_reap(pid):
            reaped.append(pid)

        def fake_alive(pgid):
            alive_checks.append(pgid)
            return False

        # These ids are not process groups this test owns. The hooks
        # stand in for kill, reap, and group existence so cleanup can
        # exercise a failed process-table read without signaling them.
        global _ps_hook, _signal_group_hook, _reap_hook, _pgid_alive_hook
        _ps_hook = slow_ps
        _signal_group_hook = fake_signal
        _reap_hook = fake_reap
        _pgid_alive_hook = fake_alive
        try:
            t_ps = time.monotonic()
            _stop_registered([_Pid(pid) for pid in synthetic], budget=1.0)
            ps_dt = time.monotonic() - t_ps
        finally:
            _ps_hook = None
            _signal_group_hook = None
            _reap_hook = None
            _pgid_alive_hook = None
        if calls["n"] > 2:
            fails.append("failing ps ran %d times (want at most 2)" % calls["n"])
        if ps_dt >= 2.0:
            fails.append("failing ps cleanup took %.2fs with budget 1s" % ps_dt)
        if [pgid for pgid, sig in signaled] != synthetic or any(
                sig != signal.SIGKILL for _pgid, sig in signaled):
            fails.append("synthetic cleanup signaled %s" % signaled)
        if sorted(set(reaped)) != synthetic or sorted(set(alive_checks)) != synthetic:
            fails.append("synthetic cleanup did not reap and probe exactly "
                         "the fake groups (reap=%s alive=%s)" %
                         (reaped, alive_checks))

        def _cancel_n(n):
            pidfile = os.path.join(td, "scale-%d.pids" % n)
            open(pidfile, "w").close()
            tsv = os.path.join(td, "scale-%d.tsv" % n)
            env = os.environ.copy()
            env.update({
                "EFS_POSIX_HOLD": "start",
                "EFS_POSIX_HOLD_S": "60",
                "EFS_POSIX_SPAWN_PID": pidfile,
            })
            proc = subprocess.Popen(
                [sys.executable, os.path.abspath(__file__), mnt,
                 "--jobs", str(n), "--timeout-s", "60", "--results", tsv],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True)
            try:
                deadline = time.monotonic() + 45
                while time.monotonic() < deadline:
                    got = [ln for ln in open(pidfile) if ln.strip()]
                    if len(got) >= n or proc.poll() is not None:
                        break
                    time.sleep(0.05)
                got = [ln for ln in open(pidfile) if ln.strip()]
                if len(got) < n:
                    fails.append("scale %d: started %d" % (n, len(got)))
                    return
                os.kill(proc.pid, signal.SIGTERM)
                try:
                    rc_n = proc.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    fails.append("scale %d: cleanup exceeded 4s" % n)
                    return
                if rc_n != 128 + signal.SIGTERM:
                    fails.append("scale %d: rc=%s" % (n, rc_n))
                alive = _pids_alive(pidfile)
                if alive:
                    fails.append("scale %d: left %s" % (n, alive))
                text = open(tsv).read() if os.path.isfile(tsv) else ""
                if "TypeError" in text or "runner exception" in text:
                    fails.append("scale %d: exception fallback" % n)
                if "# complete" in text:
                    fails.append("scale %d: interrupted run marked complete" % n)
                if not text:
                    fails.append("scale %d: no results file" % n)
            finally:
                if proc.poll() is None:
                    try:
                        os.kill(proc.pid, signal.SIGKILL)
                    except OSError:
                        pass
                    try:
                        proc.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        pass
                for tok in open(pidfile).read().split():
                    _kill_pgid(int(tok))

        for n_workers in (1, 16, 17):
            _cancel_n(n_workers)
    finally:
        shutil.rmtree(mnt, ignore_errors=True)

    if fails:
        for msg in fails:
            print("FAIL " + msg)
        return 1
    print("posix_suite self-test: pass")
    return 0


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    if args == ["--self-test"]:
        return _self_test()
    if args[:1] == ["--worker"]:
        if len(args) != 4:
            print("usage: --worker NAME TDIR RESULT", file=sys.stderr)
            return 2
        _worker_main(args[1], args[2], args[3])
        return 1
    parsed = _parse_cli(args)
    if parsed is None:
        print(__doc__)
        return 2
    (mnt, results_file, keep, stop, filt, tag,
     test_timeout, jobs) = parsed

    if not os.path.isdir(mnt):
        print("ERROR: %s is not a mounted directory" % mnt)
        return 2

    def leave_fuse_cwd():
        """Exit closes cwd. A cwd on the FUSE mount left 008–015 hung in
        request_wait_answer after the TSV was written (9-way --keep)."""
        mnt_abs = os.path.realpath(mnt)
        for t in (os.path.expanduser("~"), "/tmp", "/"):
            try:
                if not t or not os.path.isdir(t):
                    continue
                os.chdir(t)
                cwd = os.path.realpath(os.getcwd())
                if cwd != mnt_abs and not cwd.startswith(mnt_abs + os.sep):
                    return
            except OSError:
                continue

    leave_fuse_cwd()
    host = subprocess.run(["hostname", "-s"], stdout=subprocess.PIPE
                          ).stdout.decode().strip()
    # Unique per-run base (mkdtemp). Do NOT sweep leftover posix-<host>-*
    # trees: rmtree of a 4x/9-way leftover is tens of thousands of unlinks
    # and ate the 180s SSH budget (empty TSV), and a same-dir rename of
    # those trees EIO'd on the long-lived scratch. Collision is impossible
    # because the suffix is random. The --tag suffix still isolates
    # POSIX_PER_HOST siblings (each mkdtemp prefix is host+tag).
    # Testdirs under the base are created lazily in invoke() — 201
    # sequential mkdirs at ~0.3s each was another 60s before any test ran.
    matched = [(n, fn) for n, fn in TESTS if not filt or filt in n]
    if not matched:
        what = "filter %r" % filt if filt else "the suite"
        print("ERROR: no tests matched %s" % what)
        return 2
    me = "posix-%s-" % host if not tag else "posix-%s-%s-" % (host, tag)
    base = tempfile.mkdtemp(prefix=me, dir=mnt)
    npass = nfail = nskip = nnotrun = 0
    t0 = time.time()
    selected = []
    for name, fn in matched:
        tdir = (mnt if getattr(fn, "_posix_root", False)
                else os.path.join(base, name))
        selected.append((name, fn, tdir))

    tsv_mu = threading.Lock()
    by_name = {}

    def flush_tsv(done=False):
        if not results_file:
            return
        if os.environ.get("EFS_POSIX_TSV_FAIL"):
            raise OSError(errno.EIO, "injected tsv failure")
        ordered = [(n, by_name[n][0], by_name[n][1])
                   for n, _fn, _td in selected if n in by_name]
        dt = time.time() - t0
        tmp = results_file + ".tmp"
        with open(tmp, "w") as f:
            f.write("# posix-suite host=%s mnt=%s %s\n" %
                    (host, mnt, time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                              time.gmtime())))
            for name, _fn, _td in selected:
                f.write("# select\t%s\n" % name)
            f.write("test\tresult\tdetail\n")
            for name, res, detail in ordered:
                f.write(format_result_line(name, res, detail))
            f.write("# summary pass=%d fail=%d skip=%d notrun=%d total=%d dur=%.1f\n" %
                    (npass, nfail, nskip, nnotrun,
                     npass + nfail + nskip + nnotrun, dt))
            # Incremental flushes omit this. A snapshot of the first PASS
            # rows is not a finished run.
            if done and len(by_name) == len(selected):
                f.write("# complete\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, results_file)

    def record(name, status, detail):
        nonlocal npass, nfail, nskip, nnotrun
        with tsv_mu:
            by_name[name] = (status, detail)
            RESULTS.append((name, status, detail))
            if status == "PASS":
                npass += 1
                print("pass %-32s" % name, flush=True)
            elif status == "SKIP":
                nskip += 1
                print("SKIP %-32s %s" % (name, detail), flush=True)
            elif status == "NOTRUN":
                nnotrun += 1
                print("NOTRUN %-32s %s" % (name, detail), flush=True)
            else:
                nfail += 1
                print("FAIL %-32s %s" % (name, detail), flush=True)
            flush_tsv()

    live = []
    stuck_run = False
    script = os.path.abspath(__file__)
    gate = _SpawnGate()

    def _reap_live():
        return _stop_registered(list(live), budget=1.0)

    def _fill_unfinished(reason):
        nonlocal nnotrun
        for name, _fn, _td in selected:
            if name not in by_name:
                by_name[name] = ("NOTRUN", reason)
                nnotrun += 1

    def _cut(signum, _frame=None):
        # Defer until an in-progress spawn has been registered. Otherwise
        # the new session is invisible to this handler and keeps running.
        if gate.defer:
            gate.pending = signum
            return
        # timeout(1) sends SIGTERM, then SIGKILL 5 s later. Kill test
        # process groups, write NOTRUN for anything not finished, and leave.
        # Do not take tsv_mu: the main thread may already hold it in record().
        # Do not delete the tree here: a group that survives SIGKILL still
        # has it as its cwd.
        survivors = _reap_live()
        fake = os.environ.get("EFS_POSIX_FAKE_SURVIVOR")
        if fake:
            survivors = list(survivors) + [fake]
        # Always publish. Skipping the write when a group still looked
        # occupied left cancel runs with exit 143 and no TSV. The marker
        # stays off; os._exit below does not reach the tree cleanup.
        reason = "suite cut by signal %d" % signum
        if survivors:
            reason += " groups still alive: %s" % ",".join(
                str(pid) for pid in survivors)
        try:
            _fill_unfinished(reason)
            flush_tsv(done=False)
        except Exception:
            pass
        os._exit(128 + signum)

    signal.signal(signal.SIGTERM, _cut)
    signal.signal(signal.SIGHUP, _cut)
    signal.signal(signal.SIGINT, _cut)

    def spawn_test(name, tdir):
        fd, result = tempfile.mkstemp(prefix="posix-result-")
        os.close(fd)
        proc = subprocess.Popen(
            [sys.executable, script, "--worker", name, tdir, result],
            start_new_session=True)
        live.append(proc)
        pidfile = os.environ.get("EFS_POSIX_SPAWN_PID")
        if pidfile:
            with open(pidfile, "a") as f:
                f.write("%d\n" % proc.pid)
                f.flush()
                os.fsync(f.fileno())
        if os.environ.get("EFS_POSIX_CANCEL_AT_SPAWN"):
            os.kill(os.getpid(), signal.SIGTERM)
        fail_after = int(os.environ.get("EFS_POSIX_SPAWN_FAIL_AFTER", "0"))
        spawn_test.n = getattr(spawn_test, "n", 0) + 1
        if fail_after and spawn_test.n > fail_after:
            raise RuntimeError("injected spawn failure")
        return proc, result

    def on_done(name, status, detail):
        record(name, status, detail)
        if stop and status == "FAIL":
            print("stopped on first fail (--stop)")
            return True
        return False

    def items_for(rows):
        out = []
        for name, fn, tdir in rows:
            timeout = getattr(fn, "_posix_timeout", test_timeout)
            out.append((name, timeout, tdir))
        return out

    try:
        # Every test runs in its own process group. A timeout kills that
        # group. Serial tests (umask/cwd) start only after every prior
        # worker has been reaped. A worker that survives SIGKILL — a FUSE
        # operation stuck in D state — ends the run as incomplete; this
        # process does not close fds or delete the tree out from under it.
        parallel = [(n, fn, td) for n, fn, td in selected
                    if not getattr(fn, "_posix_serial", False)]
        serials = [(n, fn, td) for n, fn, td in selected
                   if getattr(fn, "_posix_serial", False)]
        if parallel:
            print("parallel %d tests jobs=%d (serial %d after)" %
                  (len(parallel), max(1, jobs), len(serials)))
        stuck, _left = _run_pool(
            items_for(parallel), max(1, jobs),
            spawn_test, on_done, live=live, gate=gate, on_cancel=_cut)
        if stuck:
            stuck_run = True
        elif serials and not (stop and nfail):
            stuck, _left = _run_pool(
                items_for(serials), 1,
                spawn_test, on_done, live=live, gate=gate, on_cancel=_cut)
            if stuck:
                stuck_run = True

        # A cut run (outer `timeout` SIGTERM, or --stop) must still have one
        # row per selected test. Missing rows become "[None]" in compare.py
        # and get read as failures (W8).
        reason = ("incomplete: timed-out worker still alive"
                  if stuck_run else "not reached")
        for name, _fn, _td in selected:
            if name not in by_name:
                record(name, "NOTRUN", reason)

        # TSV / summary follow TESTS registration order.
        RESULTS[:] = [(n, by_name[n][0], by_name[n][1])
                      for n, _fn, _td in selected if n in by_name]
        try:
            flush_tsv(done=not stuck_run)
        except Exception:
            stuck_run = True
            raise
    except BaseException as exc:
        survivors = _reap_live()
        if survivors:
            stuck_run = True
        try:
            _fill_unfinished("runner exception: %s" % exc)
            flush_tsv(done=False)
        except Exception:
            pass
        # Leave the tree. Deleting it while a worker group is still alive
        # was the SIGINT / spawn-failure failure mode.
        os._exit(1)
    finally:
        leave_fuse_cwd()
        if stuck_run:
            print("left test tree (a timed-out worker is still alive): %s" % base)
        elif keep:
            print("kept test tree: %s" % base)
        else:
            shutil.rmtree(base, ignore_errors=True)

    dt = time.time() - t0
    total = npass + nfail + nskip + nnotrun
    print("=" * 60)
    print("POSIX suite: %d/%d pass, %d fail, %d skip, %d notrun  (%.1fs)" %
          (npass, total, nfail, nskip, nnotrun, dt))

    if results_file:
        print("wrote %s" % results_file)

    # Interpreter teardown closes leftover FUSE fds (cwd, listdir,
    # leaked test fds). That sat in request_wait_answer after the TSV
    # was already written (9-way 008–015). Leave the mount and _exit.
    leave_fuse_cwd()
    try:
        mnt_abs = os.path.realpath(mnt)
        for n in os.listdir("/proc/self/fd"):
            try:
                fd = int(n)
            except ValueError:
                continue
            if fd < 3:
                continue
            try:
                tgt = os.readlink("/proc/self/fd/%d" % fd)
            except OSError:
                continue
            if not tgt.startswith("/"):
                continue
            try:
                real = os.path.realpath(tgt)
            except OSError:
                continue
            if real == mnt_abs or real.startswith(mnt_abs + os.sep):
                try:
                    os.close(fd)
                except OSError:
                    pass
    except OSError:
        pass
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(0 if nfail == 0 and nnotrun == 0 else 1)


if __name__ == "__main__":
    sys.exit(main())
