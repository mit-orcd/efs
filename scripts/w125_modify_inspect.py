#!/usr/bin/env python3
"""w125_modify_inspect.py ROOT NAME... - which w125_concurrent.py version
each region of a mismatching file holds.

For each NAME (relative to ROOT, as printed by w125_concurrent.py) the file
is read and compared, in 64 KiB blocks, against every version 0..6. Prints
the file size, the versions whose size matches, and runs of blocks with the
versions they match ('-' for none, 'z' for all zero), e.g.
  [0,8) v3  [8,9) v2  [9,19) v3
so a stale chunk or a torn update shows as a run of an older version.
"""
import os, sys
args = sys.argv[1:]
here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, "w125_concurrent.py")).read()
# Only the content model (body/op_of/apply_op/contents): cut before argparse.
ns = {}
exec(src[:src.index("ap = argparse.ArgumentParser")], ns)
contents, MAXV = ns["contents"], ns["MAX_VERSIONS"]
B = 64 << 10
root, names = args[0], args[1:]
for name in names:
    try:
        got = open(os.path.join(root, name), "rb").read()
    except OSError as e:
        print(name, "read error", e); continue
    vs = [contents(name, v) for v in range(MAXV + 1)]
    same = [v for v in range(MAXV + 1) if len(vs[v]) == len(got)]
    exact = [v for v in range(MAXV + 1) if vs[v] == got]
    runs, cur, start = [], None, 0
    nb = (len(got) + B - 1) // B
    for b in range(nb):
        blk = got[b * B:(b + 1) * B]
        m = [v for v in range(MAXV + 1) if vs[v][b * B:(b + 1) * B] == blk]
        tag = ("z" if not blk.strip(b"\0") else "-") if not m else "v" + "/".join(map(str, m))
        if tag != cur:
            if cur is not None: runs.append("[%d,%d) %s" % (start, b, cur))
            cur, start = tag, b
    if cur is not None: runs.append("[%d,%d) %s" % (start, nb, cur))
    print("%s size=%d exact=%s size_match=%s sizes=%s" % (name, len(got), exact, same, [len(v) for v in vs]))
    print("  " + "  ".join(runs))
