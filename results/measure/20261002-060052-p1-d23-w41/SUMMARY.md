# SUMMARY — results/measure/20261002-060052-p1-d23-w41

P1 of the Oct 2 performance plan: D23 (clean dcache body → rdcache) and W41
(per-inode REPORT, no shared report_mu) — gate record with A/B against the old
client build, a wedged pass-1, and four follow-up passes.

## Purpose and linked item

- **D23** (dcache body hand-off to rdcache) + **W41** (per-inode REPORT).
  Client-side changes: `src/client/write.c`, `client_internal.h`,
  `node_cache.c` — the dirty set becomes per inode (hash of inode sets).

## Verdict

**PASS** (final passes): gates pass after the wedged pass 1 (§2.1,
`deadlock-gdb-threads.txt`). before-* = old client (fcstor008, 06:00Z);
after-* = P1 pass 1 (wedged); -v2..-v5 = passes 2–5; gate-passN-*.log =
driver logs (`~/efs-runs/p1gate2.sh`); traces/ = single-test
EFS_DCACHE_TRACE=1 runs; wedge-*-fcstor010* = same-host A/B (§4); rss-* = D23
RSS gate (§3).

## Source commit and build identity

Servers `2b5a25df419c-dirty` (unchanged all day); clients fcstor008/009 on the
P1 tree, fcstor010 old build for A/B then P1 tree.

## Environment

RDMA, table from the Oct 1 07:18Z raft-mkfs; run window 06:00Z–13:30Z.

## Method

A/B old vs P1 client on the same hosts; per-pass driver logs; dcache traces;
RSS gate; gdb thread capture for the pass-1 wedge.

## Results

W41/W41-per-inode REPORT and D23 gates pass on passes 2–5; the pass-1 deadlock
(wedged) is captured in `deadlock-gdb-threads.txt` and fixed before the later
passes.

## Limitations and remaining gates

The pass-1 deadlock was real; closure is the v2–v5 pass logs, not pass 1.

## Evidence index

- `SUMMARY.txt` — gate record (run `shasum -a 256 SUMMARY.txt | cut -c1-16`
  here)
- `deadlock-gdb-threads.txt` (pass-1 wedge), `gate-passN-*.log`, `traces/`,
  `rss-*`
