#!/usr/bin/env python3
"""ecopy's copy sequence with nanosecond times: open, write, pwrite,
futimens(atime.ns, mtime.ns), close, stat, rename, stat. Prints atime/mtime
after each step; WRONG if either differs from the set value (including ns)."""
import os, sys
d = sys.argv[1]
A = 1780070101 * 10**9 + 301824970   # 2026-05-29T15:55:01.301824970Z
M = 1733413852 * 10**9 + 314886393   # 2024-12-05T15:50:52.314886393Z
os.makedirs(d, exist_ok=True)

def t(p):
    st = os.stat(p)
    return "a=%d.%09d m=%d.%09d" % (st.st_atime_ns // 10**9, st.st_atime_ns % 10**9,
                                    st.st_mtime_ns // 10**9, st.st_mtime_ns % 10**9)

def case(name, big):
    p = os.path.join(d, name + ".tmp")
    for q in (p, os.path.join(d, name)):
        try:
            os.unlink(q)
        except FileNotFoundError:
            pass
    fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o664)
    os.write(fd, b"x" * (32768 if big else 20480))
    os.pwrite(fd, b"y" * 2712, 32768 if big else 20480)
    os.utime(fd, ns=(A, M))
    os.close(fd)
    s1 = t(p)
    q = os.path.join(d, name)
    os.rename(p, q)
    s2 = t(q)
    st = os.stat(q)
    ok = st.st_atime_ns == A and st.st_mtime_ns == M
    print("%-10s after-close %s | after-rename %s -> %s" % (name, s1, s2, "OK" if ok else "WRONG"))

for i in range(5):
    case("small%d" % i, False)
    case("big%d" % i, True)
