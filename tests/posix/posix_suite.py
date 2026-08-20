#!/usr/bin/env python3
"""Comprehensive POSIX correctness suite for an efs FUSE mount.

Reusable + extensible: each test is a function decorated with @test that
receives a fresh empty directory inside the mount. Add a new test by writing
a function and decorating it — the runner picks it up automatically.

Covers both "possible" operations (must succeed) and "impossible" ones
(must fail with a specific errno), using a mix of direct syscalls and real
terminal commands (mkdir, ln, dd, cp, mv, stat, ...) via subprocess.

Usage:
    posix_suite.py <mount-dir> [--results <file>] [--keep] [--filter <substr>]

Exit code: 0 if every selected test passes, 1 otherwise.

Two-client visibility (create on A, see on B without remount) lives in
posix_2client.py — run via `tests/run_tests.sh posix2`.
"""
import errno
import fcntl
import os
import shutil
import stat as statmod
import subprocess
import sys
import tempfile
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


class Fail(Exception):
    """Raise to fail a test with a message. soft=True marks it SKIP (feature
    unsupported) rather than a hard failure."""

    def __init__(self, msg, soft=False):
        super().__init__(msg)
        self.soft = soft


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
    sh("dd if=/dev/zero of=f bs=4k count=8 2>/dev/null", cwd=d)
    eq(os.path.getsize(os.path.join(d, "f")), 32768, "dd size")
    r = sh("dd if=f bs=4k count=8 2>/dev/null | wc -c", cwd=d)
    eq(r.stdout.strip(), b"32768", "dd read bytes")


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
def dir_many_files(d):
    n = 300
    for i in range(n):
        wr(os.path.join(d, "f%04d" % i), b"x")
    eq(len(os.listdir(d)), n, "many files listed")
    for i in range(n):
        os.unlink(os.path.join(d, "f%04d" % i))
    eq(len(os.listdir(d)), 0, "many files removed")


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


def _write_script(path):
    with open(path, "w") as f:
        f.write("#!%s\nimport sys\nsys.exit(0)\n" % sys.executable)


@test
def perm_file_000_denied(d):
    if _root():
        return
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
    if _root():
        return
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
    if _root():
        return
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
    if _root():
        return
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.chmod(p, 0o007)   # --- --- rwx : owner has nothing
    expect_err(errno.EACCES, open, p, "rb")
    expect_err(errno.EACCES, open, p, "wb")
    assert not os.access(p, os.R_OK | os.W_OK | os.X_OK)


@test
def perm_owner_bits_not_group(d):
    if _root():
        return
    p = os.path.join(d, "f")
    wr(p, b"x")
    os.chmod(p, 0o070)
    expect_err(errno.EACCES, open, p, "rb")
    assert not os.access(p, os.R_OK)


@test
def perm_create_mode_000_reopen(d):
    if _root():
        return
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_WRONLY, 0o000)
    os.write(fd, b"x")
    os.close(fd)
    eq(statmod.S_IMODE(os.stat(p).st_mode), 0o000, "create mode 000")
    expect_err(errno.EACCES, open, p, "rb")
    expect_err(errno.EACCES, open, p, "wb")


@test
def perm_open_fd_survives_chmod(d):
    if _root():
        return
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
    if _root():
        return
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
    if _root():
        return
    p = os.path.join(d, "f")
    wr(p, b"x")
    expect_err((errno.EPERM, errno.EACCES), os.chown, p, os.getuid() + 1, -1)
    eq(os.stat(p).st_uid, os.getuid(), "uid unchanged")


@test
def perm_chown_other_gid_denied(d):
    if _root():
        return
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
    if _root():
        return
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
def perm_dir_no_x_search(d):
    if _root():
        return
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
    if _root():
        return
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
    if _root():
        return
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
    if _root():
        return
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
    if _root():
        return
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
    if _root():
        return
    t = os.path.join(d, "t")
    wr(t, b"x")
    os.symlink("t", os.path.join(d, "sl"))
    os.chmod(t, 0o000)
    expect_err(errno.EACCES, open, os.path.join(d, "sl"), "rb")
    os.lstat(os.path.join(d, "sl"))   # lstat must still work


@test
def perm_hardlink_shares_mode(d):
    if _root():
        return
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
    eq(len(os.listdir(d)), 0, "all crazy names removed")


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


@test
def flock_two_proc_exclusive(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    snippet = (
        "import fcntl,os,sys,time\n"
        "fd=os.open(sys.argv[1], os.O_RDWR)\n"
        "fcntl.flock(fd, fcntl.LOCK_EX)\n"
        "time.sleep(1.2)\n"
        "fcntl.flock(fd, fcntl.LOCK_UN)\n"
        "os.close(fd)\n"
    )
    child = subprocess.Popen([sys.executable, "-c", snippet, p])
    time.sleep(0.2)
    fd = os.open(p, os.O_RDWR)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            held = True
        except OSError as e:
            if e.errno not in (errno.EAGAIN, errno.EACCES):
                raise Fail("parent lock errno %s" % e)
            held = False
        if held:
            fcntl.flock(fd, fcntl.LOCK_UN)
            raise Fail("parent got LOCK_EX|NB while child held LOCK_EX")
    finally:
        os.close(fd)
        child.wait()


@test
def fcntl_byte_range_lock(d):
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
                return
            raise Fail("overlapping lockf: %s" % e)
        raise Fail("overlapping lockf both succeeded (range locks look like no-ops)")
    finally:
        try:
            fcntl.lockf(fd1, fcntl.LOCK_UN, 50, 0)
        except OSError:
            pass
        os.close(fd1)
        os.close(fd2)


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


@test
def concurrent_appends(d):
    import threading
    p = os.path.join(d, "f")
    wr(p, b"")

    def worker(i):
        for _ in range(50):
            with open(p, "ab") as f:
                f.write(b"%d\n" % i)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    lines = rd(p).splitlines()
    eq(len(lines), 4 * 50, "all appends landed (O_APPEND atomic)")


@test
def concurrent_appends_two_proc(d):
    p = os.path.join(d, "f")
    wr(p, b"")
    snippet = (
        "import os,sys\n"
        "fd=os.open(sys.argv[1], os.O_WRONLY|os.O_APPEND)\n"
        "for _ in range(40):\n"
        "    os.write(fd, (sys.argv[2]+chr(10)).encode())\n"
        "os.close(fd)\n"
    )
    procs = [
        subprocess.Popen([sys.executable, "-c", snippet, p, str(i)])
        for i in range(3)
    ]
    for pr in procs:
        if pr.wait() != 0:
            raise Fail("append child rc=%d" % pr.returncode)
    eq(len(rd(p).splitlines()), 3 * 40, "two-process O_APPEND all landed")


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
        subprocess.Popen([sys.executable, "-c", snippet, d, str(i)])
        for i in range(3)
    ]
    for pr in procs:
        if pr.wait() != 0:
            raise Fail("create/unlink child rc=%d" % pr.returncode)
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
    p = os.path.join(d, "f")
    fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        os.posix_fallocate(fd, 0, 4096)
    except OSError as e:
        if e.errno in (errno.EOPNOTSUPP, errno.ENOTSUP):
            os.close(fd)
            return
        os.close(fd)
        raise Fail("posix_fallocate: %s" % e)
    sz = os.fstat(fd).st_size
    os.close(fd)
    if sz < 4096:
        raise Fail("fallocate size %d, want >= 4096" % sz)


@test
def opt_xattr(d):
    p = os.path.join(d, "f")
    wr(p, b"x")
    try:
        os.setxattr(p, "user.efs", b"1")
        eq(os.getxattr(p, "user.efs"), b"1", "xattr roundtrip")
    except OSError as e:
        if e.errno in (errno.EOPNOTSUPP, errno.ENOTSUP, errno.EACCES):
            return
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
            return
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


# Two-client live visibility is posix_2client.py (run_tests.sh posix2).
# Still uncovered there: cross-client flock, and unlink-while-peer-has-fd.
#
# ==========================================================================
# Runner
# ==========================================================================
def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    mnt = args[0]
    results_file = None
    keep = False
    filt = None
    i = 1
    while i < len(args):
        if args[i] == "--results":
            results_file = args[i + 1]
            i += 2
        elif args[i] == "--keep":
            keep = True
            i += 1
        elif args[i] == "--filter":
            filt = args[i + 1]
            i += 2
        else:
            i += 1

    if not os.path.isdir(mnt):
        print("ERROR: %s is not a mounted directory" % mnt)
        return 2

    base = tempfile.mkdtemp(prefix="posix-", dir=mnt)
    npass = nfail = nskip = 0
    t0 = time.time()
    try:
        for name, fn in TESTS:
            if filt and filt not in name:
                continue
            tdir = os.path.join(base, name)
            os.makedirs(tdir, exist_ok=True)
            try:
                fn(tdir)
            except Fail as e:
                if getattr(e, "soft", False):
                    nskip += 1
                    RESULTS.append((name, "SKIP", str(e)))
                    print("SKIP %-32s %s" % (name, e))
                else:
                    nfail += 1
                    RESULTS.append((name, "FAIL", str(e)))
                    print("FAIL %-32s %s" % (name, e))
            except Exception as e:  # noqa: BLE001
                nfail += 1
                RESULTS.append((name, "FAIL", "%s: %s" % (type(e).__name__, e)))
                print("FAIL %-32s %s: %s" % (name, type(e).__name__, e))
            else:
                npass += 1
                RESULTS.append((name, "PASS", ""))
                print("pass %-32s" % name)
    finally:
        if keep:
            print("kept test tree: %s" % base)
        else:
            shutil.rmtree(base, ignore_errors=True)

    dt = time.time() - t0
    total = npass + nfail + nskip
    print("=" * 60)
    print("POSIX suite: %d/%d pass, %d fail, %d skip  (%.1fs)" %
          (npass, total, nfail, nskip, dt))

    if results_file:
        host = subprocess.run(["hostname", "-s"], stdout=subprocess.PIPE
                              ).stdout.decode().strip()
        with open(results_file, "w") as f:
            f.write("# posix-suite host=%s mnt=%s %s\n" %
                    (host, mnt, time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                              time.gmtime())))
            f.write("test\tresult\tdetail\n")
            for name, res, detail in RESULTS:
                f.write("%s\t%s\t%s\n" % (name, res, detail.replace("\n", " ")))
            f.write("# summary pass=%d fail=%d skip=%d total=%d dur=%.1f\n" %
                    (npass, nfail, nskip, total, dt))
        print("wrote %s" % results_file)

    return 0 if nfail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
