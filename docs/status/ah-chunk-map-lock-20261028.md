# AH — one mutex for every chunk-map lookup and update on a mount (2026-10-28)

**Report:** (first paragraph filled at the end: branch + head, gate line,
NOTRUN list, one line per product defect.)

## Purpose and item

Checklist `alpha-1.0-2plus1-checklist.md` AB–AX, item **AH — "Mount-wide
chunk-map read/write contention"**: `g_client.idx_mu` is one mutex for every
chunk-map lookup and update on a mount (~112 sites across
`src/client/read.c`, `write.c`, `ops.c`, `efs_fuse.c`), required because the
map arrays `realloc` under set/reserve and unpinned pointers would be
use-after-free. The data caches are already striped. The question was
contention; only a measurement answers it.

## Verdict: PASS — measured, no change

Contention events are common but each resolves in microseconds, and the
total wait share stays far below anything that justifies partitioning:

| writers | phase | acquisitions/s | trylock-miss share | wait per wall |
| --- | --- | --- | --- | --- |
| 1 | fixed set | ~2,400 | 3.8 % | 0.005 % |
| 1 | map growth | ~5,600 | 5.2 % | 0.02 % |
| 8 | fixed set | ~2,500 | 17.2 % | 0.04 % |
| 8 | map growth | ~4,700 | 15.3 % | 0.02 % |
| 32 | fixed set | ~4,200 | 25.5 % | 0.15 % |
| 32 | map growth | ~4,600 | 29.6 % | 0.08 % |

Acquisitions stay flat with writer count because the workload's own rate
(a 64 KiB write + fsync per op on a loopback cluster) is the limiter, not
the lock; at 32 writers about a quarter of acquisitions miss the trylock
but the median contended wait is ≈0.35 µs and the aggregate wait is at most
0.15 % of wall time. Stated threshold for "above": a wait share of about
1 % of wall or contention visibly capping throughput — the measurement is
two orders below. **Recorded; nothing partitioned.**

## Method

- Host xorinox, 3 production `efsd` daemons on loopback (ports 19411–19413,
  free per `ss -ltn`), stores under `~/efs-taskAH/`, TCP.
- `scripts/ah_measure.py` (committed) builds a shim copy of the tree —
  never the working tree — where each `pthread_mutex_lock/unlock
  (&g_client.idx_mu)` becomes a counting wrapper (acquisitions, trylock
  misses, wait ns, a 2 s report; the shim source is embedded in the script
  and is a local experiment, never linked into production). Load: W writer
  threads at W ∈ {1, 8, 32} over FIXED (rewrites of a pre-created set, no
  map growth) and GROWTH (new files while readers sweep the fixed set).
- The base tree is devel `9c4bfd171`; the branch carries the measurement
  script and docs only — zero product diff, so the gate's product behavior
  is the base's by construction.

## Gates

- `tests/test_sparse_metadata.py` (the unchanged map's existing gate):
  PASS on xorinox.
- TSan client tests (`make tsan-check`): 19/19 PASS, rc=0.
- `make test-all` on xorinox at the head: (filled when it lands).

## Artifacts

Raw measurement output (the per-phase sample lines, the shim tree's log)
and the gate summary: xorinox `~/efs-artifacts/taskAH-20261028/` with
`SHA256SUMS`; the shim copy of the tree is recreated by
`scripts/ah_measure.py` from any checkout, so it was deleted after the
runs (`PRUNED.txt` in the artifacts directory).
