#!/usr/bin/env python3
"""Two-client POSIX visibility suite.

Single-mount posix_suite.py cannot see this: each efs-fuse keeps its own
metadata snapshot, and Phase 2b only dual-applies a mutation on the client
that issued it. A peer does not see mkdir/create/rename until remount
(Phase 2c). That is the IO-500 IOR-hard failure: all ranks O_CREAT one
shared file; the creator's peers get EIO/ENOENT instead of the inode.

Each test is a sequence of steps on side A then B (same export path, two
mounts / two hosts). B is never remounted during a test.

Usage (on one host, two paths — XFS baseline uses the same path twice):
    posix_2client.py --local <mnt-a> <mnt-b> [--results FILE] [--filter SUB]

Usage (login node, two efs-fuse hosts; parent dirs must already be visible
on B — run --prepare on A, remount B, then --remote):
    posix_2client.py --prepare <mnt>
    posix_2client.py --remote <host-a> <host-b> --mnt <mnt> [--results FILE]

Internal (harness / --remote):
    posix_2client.py --exec <test> <a|b> <dir>
"""
from __future__ import print_function

import errno
import os
import shutil
import subprocess
import sys
import time

PARENT = "posix-2c"
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
    os.makedirs(base)
    for name, _steps, _doc in TESTS:
        os.makedirs(os.path.join(base, name))
    print("prepared %s (%d testdirs)" % (base, len(TESTS)))


def run_step_index(name, idx, d):
    steps = None
    for n, s, _doc in TESTS:
        if n == name:
            steps = s
            break
    if steps is None:
        raise Fail("unknown test %s" % name)
    if idx < 0 or idx >= len(steps):
        raise Fail("bad step index %d for %s" % (idx, name))
    _side, fn = steps[idx]
    fn(d)


def write_results(path, host_a, host_b, mnt, npass, nfail, dt):
    with open(path, "w") as f:
        f.write("# posix-2client host_a=%s host_b=%s mnt=%s %s\n" %
                (host_a, host_b, mnt,
                 time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())))
        f.write("test\tresult\tdetail\n")
        for name, res, detail in RESULTS:
            f.write("%s\t%s\t%s\n" %
                    (name, res, detail.replace("\n", " ")))
        f.write("# summary pass=%d fail=%d skip=0 total=%d dur=%.1f\n" %
                (npass, nfail, npass + nfail, dt))
    print("wrote %s" % path)


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
                print("FAIL %-32s testdir missing on B" % name)
                continue
            try:
                for side, fn in steps:
                    fn(da if side == "a" else db)
            except Fail as e:
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


def ssh_cmd(ssh, host, remote):
    r = subprocess.run([ssh, host, remote],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = r.stdout.decode("utf-8", "replace")
    err = r.stderr.decode("utf-8", "replace")
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
                host = host_a if side == "a" else host_b
                cmd = "python3 %s --exec %s %d %s" % (
                    _q(script), _q(name), i, _q(d))
                rc, out, err = ssh_cmd(ssh, host, cmd)
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
        except Fail as e:
            nfail += 1
            RESULTS.append((name, "FAIL", str(e)))
            print("FAIL %-32s %s" % (name, e))
        else:
            npass += 1
            RESULTS.append((name, "PASS", ""))
            print("pass %-32s" % name)

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
        # --exec <test> <step-index> <dir>
        if len(args) < 4:
            print("usage: --exec <test> <step-index> <dir>")
            return 2
        name, idx_s, d = args[1], args[2], args[3]
        try:
            run_step_index(name, int(idx_s), d)
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
