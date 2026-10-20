#!/usr/bin/env python3
"""Files created per 60 s of a w125_p0_capture.py CSV (column 5 = create start, us)."""
import csv, sys
t = sorted(int(r[4]) for r in csv.reader(open(sys.argv[1])) if r and r[4].isdigit())
if not t: sys.exit("no rows")
b = {}
for x in t: b[(x - t[0]) // 60000000] = b.get((x - t[0]) // 60000000, 0) + 1
print("files=%d seconds=%.1f mean=%.0f/s" % (len(t), (t[-1] - t[0]) / 1e6, len(t) / max((t[-1] - t[0]) / 1e6, 1e-9)))
for k in sorted(b): print("minute %d %.0f/s" % (k, b[k] / 60.0))
