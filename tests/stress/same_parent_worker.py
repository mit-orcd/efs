#!/usr/bin/env python3
"""same_parent_storm.sh worker: PROCS forked children, each ROUNDS times
mkdir d / create f / rmdir d / unlink f in ONE parent. Names are unique per
(host, proc, round), so every conflict is on the parent row and its dseq
emptiness witness — the §7.2 reduce path — never on a dentry."""
import errno
import os
import sys
import time

parent, procs, rounds, host = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]


def run(p):
    errs = 0
    t0 = time.time()
    for r in range(rounds):
        d = f"{parent}/d-{host}-{p}-{r}"
        f = f"{parent}/f-{host}-{p}-{r}"
        ops = (
            ("mkdir", lambda: os.mkdir(d)),
            ("create", lambda: os.close(os.open(f, os.O_CREAT | os.O_WRONLY, 0o644))),
            ("rmdir", lambda: os.rmdir(d)),
            ("unlink", lambda: os.unlink(f)),
        )
        for op, fn in ops:
            try:
                fn()
            except OSError as e:
                errs += 1
                print(f"ERR {host} p{p} r{r} {op} {errno.errorcode.get(e.errno, e.errno)}", flush=True)
    print(f"DONE {host} p{p} rounds={rounds} errs={errs} secs={time.time() - t0:.1f}", flush=True)
    return errs


kids = []
for i in range(procs):
    pid = os.fork()
    if pid == 0:
        os._exit(1 if run(i) else 0)
    kids.append(pid)
bad = 0
for pid in kids:
    _, st = os.waitpid(pid, 0)
    bad += 1 if os.waitstatus_to_exitcode(st) else 0
print(f"HOST_DONE {host} bad_procs={bad}", flush=True)
sys.exit(1 if bad else 0)
