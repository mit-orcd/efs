#!/usr/bin/env python3
"""write -> futimens -> close -> stat, versus write -> close -> utimens -> stat,
versus write -> futimens -> fsync -> close.  Prints the mtime after each step."""
import os, sys, time
d = sys.argv[1]
T = (1380661863, 1380661863)  # 2013-10-01T21:11:03Z
os.makedirs(d, exist_ok=True)

def mt(p):
    st = os.stat(p)
    return "%d.%09d" % (st.st_mtime_ns // 10**9, st.st_mtime_ns % 10**9)

def case(name, steps):
    p = os.path.join(d, name)
    try:
        os.unlink(p)
    except FileNotFoundError:
        pass
    fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    out = []
    for s in steps:
        if s == 'write':
            os.pwrite(fd, b"000002\n", 0)
        elif s == 'futimens':
            os.utime(fd, ns=(T[0] * 10**9, T[1] * 10**9))
        elif s == 'fsync':
            os.fsync(fd)
        elif s == 'close':
            os.close(fd); fd = -1
        elif s == 'utimens':
            os.utime(p, ns=(T[0] * 10**9, T[1] * 10**9))
        elif s == 'rename':
            q = p + ".final"; os.rename(p, q); p = q
        out.append("%s=%s" % (s, mt(p)))
    if fd >= 0:
        os.close(fd)
    st = os.stat(p)
    ok = st.st_mtime_ns == T[1] * 10**9
    print("%-32s %s  -> final %s %s" % (name, " ".join(out), mt(p), "OK" if ok else "WRONG"))

case("ecopy-order", ['write', 'futimens', 'close', 'rename'])
case("ecopy-order-nosleep-stat", ['write', 'futimens', 'close'])
case("rsync-order", ['write', 'close', 'utimens'])
case("fsync-between", ['write', 'futimens', 'fsync', 'close'])
case("futimens-then-write", ['futimens', 'write', 'close'])
time.sleep(1)
print("after 1 s:")
for n in ("ecopy-order.final", "ecopy-order-nosleep-stat", "rsync-order", "fsync-between", "futimens-then-write"):
    print("  %-28s %s" % (n, mt(os.path.join(d, n))))
