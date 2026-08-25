#!/usr/bin/env python3
"""One-client half of unlink_storm.sh: create, wait for go, rmtree, prove alive."""
import errno
import os
import shutil
import signal
import sys
import time


def main():
    mnt, nfiles, ndirs, rm_sec, wait_sec = (
        sys.argv[1],
        int(sys.argv[2]),
        int(sys.argv[3]),
        int(sys.argv[4]),
        int(sys.argv[5]),
    )
    host = os.uname().nodename.split(".")[0]
    # Unique dir so a leftover tree from a failed run is not rmtree'd
    # first (that was the teardown hang we are measuring).
    base = os.path.join(mnt, "ustorm-%s-%d" % (host, int(time.time())))
    for i in range(ndirs):
        os.makedirs(os.path.join(base, str(i)), exist_ok=True)
    t0 = time.time()
    for i in range(nfiles):
        p = os.path.join(base, str(i % ndirs), str(i))
        with open(p, "w") as f:
            f.write("x")
        if (i + 1) % 500 == 0:
            print("CREATE_PROG n=%d %.2fs" % (i + 1, time.time() - t0), flush=True)
    print("CREATE_OK n=%d %.2fs" % (nfiles, time.time() - t0), flush=True)
    open(os.path.join(mnt, "ustorm-ready-" + host), "w").close()
    go = os.path.join(mnt, "ustorm-go")
    deadline = time.time() + wait_sec
    while not os.path.exists(go):
        if time.time() > deadline:
            print("TIMEOUT_GO", flush=True)
            return 2
        time.sleep(0.05)

    def _to(_s, _f):
        raise TimeoutError("rmtree")

    signal.signal(signal.SIGALRM, _to)
    signal.alarm(rm_sec)
    t0 = time.time()
    def _onerr(func, path, exc_info):
        err = exc_info[1]
        if getattr(err, "errno", None) == errno.ENOENT:
            return
        raise err

    try:
        last = None
        for _ in range(8):
            try:
                shutil.rmtree(base, onerror=_onerr)
                last = None
                break
            except OSError as e:
                last = e
                if getattr(e, "errno", None) not in (errno.ENOTEMPTY,
                                                     errno.ENOENT):
                    signal.alarm(0)
                    print("RM_ERR %s" % e, flush=True)
                    return 4
                time.sleep(0.05)
        if last is not None:
            shutil.rmtree(base, ignore_errors=True)
        signal.alarm(0)
        print("RM_OK %.2fs" % (time.time() - t0), flush=True)
    except TimeoutError:
        print("RM_HANG after %.2fs" % (time.time() - t0), flush=True)
        return 3
    except OSError as e:
        print("RM_ERR %s" % e, flush=True)
        return 4
    with open(os.path.join(mnt, "ustorm-alive-" + host), "w") as f:
        f.write("ok\n")
    print("ALIVE", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
