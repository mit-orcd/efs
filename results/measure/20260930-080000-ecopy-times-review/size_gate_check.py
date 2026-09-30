#!/usr/bin/env python3
"""Stat the files size_gate.py wrote, from any client; expectations in the
JSON written by size_gate.py (copied over by the caller)."""
import os, sys, json
exp = json.load(open(sys.argv[1]))
bad = 0
for q, (sz, a, m) in sorted(exp.items()):
    try:
        st = os.stat(q)
    except OSError as e:
        print("BAD %s: %s" % (q, e)); bad += 1; continue
    if st.st_size != sz or st.st_atime_ns != a or st.st_mtime_ns != m:
        bad += 1
        if bad <= 20:
            print("BAD %s size %d/%d atime %d/%d mtime %d/%d" % (q, st.st_size, sz,
                  st.st_atime_ns, a, st.st_mtime_ns, m))
print("files=%d mismatches(other client)=%d" % (len(exp), bad))
