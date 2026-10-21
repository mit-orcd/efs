# SUMMARY — 20261008-w93-w96-implementation

## Purpose and linked item
Implementation checkpoint for three work items: **W93** (broader durable Raft
group commit / bounded follower batching), **W94** (atomic CREATE and open-lease
acquisition), **W96** (bounded preallocation and discarded-tail recovery for the
KV WAL). Status page: `docs/status/w93-w96-implementation-20261008.md`. Baseline
`devel` merge `d4e70492`; subdir manifests pin per-fixture source/binary hashes.
The run is a set of nested phase fixtures, one per subtopic.

## Verdict
**Mixed — read per phase:**
- Functional/durability gates: **PASS** (w96-recovery, w94-atomic-*,
  w94-guarded-edges, final-live, live-churn — full Linux tests, POSIX
  216/0/1, peer 64/64, cold restart, GC drain).
- W53/W93 close-tail performance: **NOT accepted** — uncontaminated baseline
  close p99 237–249 ms vs candidate 266–282 ms in w93-mixed-load; the longer
  balanced w93-mixed-long series shows mean close p99 within 1% (no consistent
  regression, no proven win).
- w93-quota-pressure: **FAIL** (excluded) — 1 GiB/node fixture hit ENOSPC when
  overwrites outpaced bounded GC; preserved stores drained cleanly but the run
  is excluded from timing.
- w96-nuc: performance acceptance **OPEN** — QD8 root2 candidate slower in two
  of three balanced pairs on a busy NUC; isolated repeat still required.
None of the four performance packages is closed by this checkpoint.

## Source commit and build identity
Baseline `d4e70492` (includes `e6926d85`). Candidate phases include W93 batching
(`54e800cd`), W96 WAL fixes (`f35a67c0a`, `0ed74ef8e`), ACK-ordering experiment
`34fd56fa`; final-live uses `97772dd2` plus cold-ownership test `9fd3ef2d`.
Exact per-fixture source/binary SHA256 are in each `manifest.json`.

## Environment
AMD host, one machine, four private daemons per fixture, direct-I/O/TCP,
private stores (`/data1/efs/gc-direct-*`), installed stores/binaries preserved
and the installed service restored after each fixture. The NUC two-device
subset (w96-nuc) used /dev/sda and an LVM volume on a busy NUC. No physical
power-loss or distributed-RDMA proof; process-crash evidence only.

## Method
Per-phase microbenchmarks plus live gates:
- `w93-batching/raft_recv_cost.c`, `raft_recv_mixed_cost.c`: 2560 one-KiB
  records, 16 already-ready requests/wave, serial vs batched recv, cold reopen
  verifies every record.
- `w96-preallocation/kv_wal_cost.c`: reserve 0 vs 1048576, QD1/QD8, 2000
  writes/thread, replay asserts exact counts.
- Mixed-load fixtures: 8 tiny workers (512–4096 512-byte CREATE/close) plus 4
  bounded 16 MiB large-file rings, abrupt daemon restart, exact byte checks,
  deletion and physical GC drain.
- Live gate: `gc_reclamation.py`-style fixture covering POSIX, peer, atomic
  CREATE leases, rejected replies, append/sync, dirty-stop recovery, WAL-tail
  repair and a second cold crash.
Cleanup: installed four-daemon/two-mount service restored after each fixture.

## Results
| Phase | Key numbers | Verdict |
| --- | --- | --- |
| w93-batching mixed-group width 16 | 6443–6706 vs 44329–49288 entries/s; p99 2584–2750 vs 420–485 µs | improvement |
| w93-batching width 1 | p99 serial 279–296 µs vs batch 276–292 µs | no fill delay |
| w93-mixed-load | close p99 baseline 237–249 ms, candidate 266–282 ms | tail NOT accepted |
| w93-mixed-long (3 balanced pairs) | mean close p99 ack 309.124 ms vs serial 311.454 ms | no consistent regression |
| w93-observed | max pump hold 15.902 ms, lock wait 23.700 ms, persistence max 1.824 ms, close p99 295.424 ms | no single stall identified |
| final-live | Linux make test PASS; POSIX 216/0/1; peer 64/64; GC/restart/tail-repair PASS | PASS |

## Limitations and remaining gates
Short closed-loop samples do not attribute the historical close-tail
difference to batching; W53 end-to-end close tails remain separate. W94 client
ownership/recovery integration, production runtime sessions and activation
gates are open. W96 was closed later (Oct 11, devel-nuc-closure-20261010.md) —
this run's w96-nuc performance data is explicitly not an unconditional
improvement. No hardware power-loss or distributed-RDMA claims.

## Evidence index
| File | sha256 (16) | Carries |
| --- | --- | --- |
| `final-live/README.md` | 1c54535eb9b2cf00 | Final live gate scope and verdict |
| `final-live/manifest.json` | 9e425980ee408d40 | Final source/binary identities |
| `final-live/posix.tsv` | 573ee2d4286829d8 | Final POSIX summary |
| `final-live/final-status.json` | baf8c41146db95a3 | Final GC/cold status |
| `w93-batching/mixed-results.jsonl` | b8e30005aa09c8a6 | Serial vs batched cost samples |
| `w93-mixed-load/README.md` | abb24efdf3457545 | Tail-gate non-acceptance |
| `w93-mixed-long/README.md` | f0b2d9edccec6a5b | Balanced long-pair table |
| `w93-observed/maxima.json` | 1c25d589ce4263b5 | Pump/lock/persistence maxima |
| `w93-quota-pressure/quota-observation.json` | e28c0dff0960a9d9 | Failed-fixture drain observation |
| `w96-nuc/README.md` | 1589c96f5193c40c | NUC two-device mixed results |
| `w96-interior-damage/README.md` | 594c938997c694da | Interior-damage guard scope |
| `w96-recovery/live.log` | 819ca7460d4c3ace | WAL tail-repair live acceptance |
