# AT — PUT-window completion wake-ups: measured, no change (Oct 28–29, 2026)

Branch `agent/devel-taskat`, head `dfd5fd191` (on devel `9c4bfd171`;
measurement assets only, no product change). Gate: `make test-all` on
xorinox = **TOTAL 546 FAIL 1 NOTRUN 11 (1134s)**; the tree is devel plus
measurement scripts, so the gate is itself the base comparison. The one FAIL
is the pre-existing devel b6 slow-mode defect (AK; identical F0 fsync-EIO
signature, write path, now seen on intel1, intel2 and xorinox). The 11
NOTRUN are host limits (no IB device, no cluster scripts, sanitizer probe).
Defects found by this task: none. Named AT gates PASS:
`tests/test_put_window_deadline.py`,
`tests/test_k38_shared_chunk_pressure.py`,
`tests/test_k35_fsync_degraded.py`.

## Verdict

**Measured, no change (record and stop).** Every close broadcasts on
`put_win_cv`, so waiters on unrelated inodes do wake often — at 32 writers
there are ~85 wake events per satisfied waiter. But zero deadline timeouts
occurred at any writer count, the wait durations track PUT completion time
(rather than wake latency), and broadcast-wake CPU is ~0.05 % of the 16-core
host: no measurable end-to-end gain is available from per-inode (or sharded)
waiter ownership, and the family's lost-wake invariant is not to be touched
for a timing claim without one.

## What was measured

`put_win_close` (src/client/write.c:4994) decrements the inode's window count
and unconditionally `pthread_cond_broadcast(&put_win_cv)` (line 5010);
`put_win_wait` (line 5028) re-checks the inode predicate under `put_win_mu`
with a 50 ms capped timed wait per slice.

Driver `scripts/evidence/at/at_measure.py` with the temporary, never committed counter
patch `scripts/evidence/at/at-instrument.patch`: private 3-daemon loopback cluster on
xorinox, one mount, writers looping create/write-1MiB/fsync/close so fsync
waits for its inode's in-flight PUT windows (1/8/32 writers, 20 s each).

| writers | fsyncs | wait calls | wakes | satisfied | wakes/satisfied | timeouts | fsync p50 | p95 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 134 | 134 | 408 | 115 | 3.5 | 0 | 121 ms | 224 ms |
| 8 | 482 | 482 | 7,111 | 397 | 17.9 | 0 | 291 ms | 400 ms |
| 32 | 559 | 559 | 20,562 | 243 | 84.6 | 0 | 947 ms | 1,954 ms |

- Wakes are driven by unrelated closes (open=close counts per phase) and the
  50 ms slices; a satisfied waiter at 32 writers wakes ≥15 times on average.
- The wait **duration** histograms move with writer count exactly like the
  fsync latency itself (2–8 ms at 1 writer, 16–128 ms at 8, 256 ms–1 s at
  32): a wait ends when the inode's PUTs complete, so its cost is PUT
  completion time, not wake-up latency. Trimming wakes cannot move it.
- Mutex reacquisitions ≈ wake count (one per timed-wait return), ~1,000/s at
  32 writers ≈ 2–10 ms CPU/s on a 16-core host (~0.01–0.06 %).
- Deadline accuracy: zero EFS_ERR_BUSY at 1/8/32 (to=0 in every phase); 316
  of 559 calls at 32 writers needed no wait at all (it0).

## Gates

Named AT gates on the clean (uninstrumented) build at `9c4bfd171` on
xorinox: `tests/test_put_window_deadline.py`,
`tests/test_k38_shared_chunk_pressure.py`,
`tests/test_k35_fsync_degraded.py` — all PASS. The
deterministic simultaneous-window/partial-close/overflow/lost-wake/shutdown
tests are implementation gates and are not written: no product change was
made. TSan runs inside test-all.

Full gate: `make test-all` on xorinox: **TOTAL 546 FAIL 1 NOTRUN 11 (1134s),
OVERBUDGET 21** — the FAIL is the pre-existing b6 slow-mode defect above.
Raw: xorinox `~/efs-artifacts/taskAT-20261029/test-all-summary.txt`.

## Reproduction

```
patch -p1 < scripts/evidence/at/at-instrument.patch   # TEMP-AT counters (never committed)
make -j16 efsd efs-fuse
python3 scripts/evidence/at/at_measure.py --src . --work /tmp/at --writers 1,8,32 --seconds 20
```

Restore `src/client/write.c` and `src/client/efs_fuse.c` afterwards. Raw
artifacts: xorinox `~/efs-artifacts/taskAT-20261029/` with `SHA256SUMS`.
Environment: xorinox VM, 16 CPUs, loopback TCP, idle host.
