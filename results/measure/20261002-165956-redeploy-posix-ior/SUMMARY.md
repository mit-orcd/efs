# Redeploy of 20745142 (lookup-memo client) + posix/posix2/IO-500 — Oct 2 2026 16:59Z

## Purpose and linked item

Gate for client build `20745142ad79-dirty` (one readdir listing per opendir;
LOOKUP memo answers the following GETATTR). The run **surfaced W54** ("a
fold's GC deletes the live base", `docs/backlog/work-items.md` queue row 0i —
MUST FIX data loss) via the IO-500 ior-hard-read EIO, and the lookup-memo
regression rows 0i/0j. Added by commit `ab4922cb0`.

## Verdict

**FAIL** — two distinct defects:

1. posix jobs=1 regressed to 164/201 with **36 EFS BUGS** (stale GETATTR from
   the 50 ms lookup memo: size 0 after write, old mode after chmod, old nlink
   after link, old mtime after utimens). Prior gate had 0.
2. IO-500 9×4 **ABORTed in ior-hard-read**: rank 34 (fcstor015) read() EIO at
   17:04:40Z, driver MPI_ABORT, no score, later phases never ran. Root cause
   recorded under W54: a publish fold queued the live base generation for GC
   and the reaper deleted the fragments the row still names; the affected
   file is unrecoverable (test data).

posix2 passed 63/63 (the peer client does not hold the memo).

## Source commit and build identity

All four efsd and the clients: `build=20745142ad79-dirty` (the `-dirty` is
untracked `results/` in the rsynced tree; the four servers match). Servers
had been `111a07527093-dirty`. Roll via `cluster.sh restart --clients
--perf`, storage kept (no wipe).

## Environment

Cluster 19810: servers fcstor003–006, RDMA, `--perf`. Clients fcstor007–015.
Same table as the Oct 1 07:18Z raft-mkfs; IO-500 9×4 debug, 36 ranks,
1 s stonewall. posix on fcstor008, posix2 on fcstor008+009. 16:59–17:09Z.

## Method

`cluster.sh restart --clients --perf` (`cluster.txt`), preflights before and
after (`preflight-before.txt`, `preflight-after.txt`), then posix jobs=1,
posix2, IO-500 9×4 debug (`ior-launch.txt`). Results landed in
`results/posix/20261002-170119`, `results/posix2/20261002-170203`,
`results/io500/20261002-165956-rdma`.

## Results

| phase | this run | 15:21Z prior | Oct 1 |
| --- | ---: | ---: | ---: |
| posix jobs=1 | 164/201, 36 fail | 200/201, 0 fail | — |
| posix2 | 63/63 (47.3 s) | 63/63 | — |
| ior-easy-write | 4.393 GiB/s | 6.837 | 5.173 |
| mdtest-easy-write | 4.573 kIOPS | 5.779 | 6.185 |
| ior-hard-write | 0.530 GiB/s (64.8 s) | 0.600 (48.6 s) | 0.519 (63.2 s) |
| mdtest-hard-write | 2.791 kIOPS | 4.154 | 2.818 |
| ior-easy-read | 25.819 GiB/s | 20.334 | 22.521 |
| mdtest-easy-stat | 19.809 kIOPS | 17.907 | 24.445 |
| ior-hard-read | **ABORT (EIO rank 34)** | 0.845, 0 err | 0.824, 1 err |

Phases after ior-hard-read did not run. SCORE [INVALID] on the phases that
ran only because stonewall is 1 s; no overall score because of the abort.
Reads that did run are same-mount.

## Limitations and remaining gates

- W54 analysis (in `docs/backlog/work-items.md`, citing this run): the base
  generation IS the second tombstone; on disk only the tombstoned generation
  remains. Hit is probabilistic (same-gen collision + GC + cache miss), so
  the 15:21Z clean hard-read does not clear it. Fix steps (a)–(e) listed in
  the W item; not fixed by this run.
- Lookup-memo regression is client-side; posix2 cannot see it (peer stats
  the other client).
- Hard file left on the mount:
  `/tmp/efs-mount/io500/2026.10.02-13.02.57/ior-hard/file` (-G=1817656206).

## Evidence index

- `SUMMARY.txt` — 67f4a46b5ed1c48d — original write-up with full table
- `cluster.txt` — 6f70c18abf796192 — cluster.sh restart transcript
- `preflight-before.txt` — ffa2fdf94c31a14f — pre-roll state
- `preflight-after.txt` — ae5277eb5ecb78e8 — post-roll state, build identity
- `posix.txt` — 99c6ac193050ad81 — posix result (36 fails)
- `posix2.txt` — 9f3d794881c6a606 — posix2 result (63/63)
- `ior-launch.txt` — 8e33cf6ef9e7c946 — IO-500 launch transcript
