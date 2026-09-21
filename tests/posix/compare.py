#!/usr/bin/env python3
"""Compare two posix_suite results files: a baseline (e.g. XFS) and a test
target (e.g. efs). Classifies each test:

  EFS-BUG    baseline PASS, target not PASS   -> a real correctness gap
  OK         both PASS
  BOTH-FAIL  both fail                        -> behaviour matches baseline
  TARGET-++  baseline not PASS, target PASS   -> target is "more correct"

Usage: compare.py <baseline-results> <target-results>
Exit code: 1 if any EFS-BUG, else 0.
"""
import sys


def load(path):
    res = {}
    detail = {}
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2 or parts[0] == "test":
                continue
            res[parts[0]] = parts[1]
            detail[parts[0]] = parts[2] if len(parts) > 2 else ""
    return res, detail


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    base, bd = load(sys.argv[1])
    targ, td = load(sys.argv[2])

    names = sorted(set(base) | set(targ))
    bugs, bothfail, better, ok, only_t, notrun = [], [], [], [], [], []
    for n in names:
        b = base.get(n)
        t = targ.get(n)
        if b is None:
            only_t.append(n)
            continue
        # A test the target never reached is a cut run, not a wrong answer.
        # Missing rows used to land here as [None] and count as EFS-BUG (W8).
        if t is None or t == "NOTRUN":
            notrun.append((n, t if t else "missing", td.get(n, "")))
            continue
        if b == "PASS" and t == "PASS":
            ok.append(n)
        elif b == "PASS" and t != "PASS":
            bugs.append((n, t, td.get(n, "")))
        elif b != "PASS" and t == "PASS":
            better.append(n)
        else:
            bothfail.append((n, b, t))

    print("=" * 64)
    print("POSIX baseline comparison: %d tests" % len(names))
    print("  both pass        : %d" % len(ok))
    print("  EFS BUGS         : %d  (baseline PASS, target FAIL)" % len(bugs))
    print("  both fail (match): %d" % len(bothfail))
    print("  target better    : %d" % len(better))
    print("  not run          : %d  (cut or missing; not a bug)" % len(notrun))
    print("=" * 64)
    if bugs:
        print("\n--- EFS BUGS (pass on baseline, fail on target) ---")
        for n, t, det in bugs:
            print("  %-32s [%s] %s" % (n, t, det))
    if better:
        print("\n--- target passes where baseline fails ---")
        for n in better:
            print("  %-32s" % n)
    if notrun:
        print("\n--- not run (suite cut or row missing; not an EFS bug) ---")
        for n, t, det in notrun:
            print("  %-32s [%s] %s" % (n, t, det))
    if bothfail:
        print("\n--- both fail (behaviour matches baseline) ---")
        for n, b, t in bothfail:
            print("  %-32s base=%s target=%s" % (n, b, t))
    return 1 if bugs else 0


if __name__ == "__main__":
    sys.exit(main())
