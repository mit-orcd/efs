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
