# AM — REPORT degraded-object bookkeeping: measured, no change (Oct 28–29, 2026)

Branch `agent/devel-taskam`, head `3fe865d98` (on devel `9c4bfd171`;
measurement assets only, no product change). Gate: `make test-all` on intel1
= **TOTAL 546 FAIL 1 NOTRUN 12 (822s)**; the tree is devel plus two
measurement scripts, so the gate is itself the base comparison. The one FAIL
is the pre-existing devel defect recorded by AK
(`EFS_B6_MODES=slow tests/test_b6_unavailable.py`: F0 fsync EIO at the 60 s
sweep deadline, siblings OK at 8 s — proven on a devel `9c4bfd171` baseline
on intel2, now reproduced on intel1 with the identical signature, so it is
host-independent; write path, unrelated to AM). The 12 NOTRUN are host
limits shared with other gates: no IB device (8 RDMA commands), no valgrind
headers, no ~/git/cluster scripts, sanitizer probe. Defects found by this
task: none. Named AM gates all PASS (k35 fsync-degraded, REPORT
partial-verdict, resolved-retry, publication/namespace session tests).

## Verdict

**Measured, no change (record and stop).** The threshold for changing
`g_dgobj_mu` is above 5 % wait share at 32 streams or a measurable end-to-end
gain. Measured worst case at 32 writers: **0.0025 %** of REPORT wall time —
two orders of magnitude below the threshold. No batch lookup/retirement is
built; the empty-table fast path and the per-record semantics named in the
checklist are untouched.

## What was measured

`efs_client_rpc_report_dirty_pub` (src/client/inode_rpc.c:3473) calls
`efs_client_degraded_flags` per serialized record and
`efs_client_degraded_drop` per record after an acknowledged REPORT; both
probe the open-addressing degraded-object table under `g_dgobj_mu`
(src/client/write.c:2852) whenever the live count is nonzero.

Driver `scripts/evidence/am/am_measure.py` (committed) with the temporary, never
committed counter patch `scripts/evidence/am/am-instrument.patch` (clock_gettime around
each acquisition, REPORT call timing histogram, `EFS_AM_SEED_DGOBJ` seeding,
SIGPROF dump — the `perf lock` tool is unavailable on intel1, so the brief's
temporary-instrumentation alternative is used): a private 3-daemon loopback
cluster on intel1, one mount per state, writer threads looping
create/write-4KiB/fsync/close.

| state | writers | fsyncs | dg_locks | wait per acq | lock-wait ÷ REPORT wall |
| --- | --- | --- | --- | --- | --- |
| empty | 1 | 1,226 | 0 | — | 0 % |
| empty | 8 | 3,002 | 0 | — | 0 % |
| empty | 32 | 4,697 | 0 | — | 0 % |
| one pending (seeded) | 1 | 1,067 | 2,134 | 296 ns | 0.027 % |
| one pending (seeded) | 8 | 2,994 | 5,988 | 285 ns | 0.0047 % |
| one pending (seeded) | 32 | 4,102 | 8,204 | 312 ns | **0.0025 %** |
| dense (8,192 seeded) | 1 | 985 | 1,970 | 319 ns | 0.029 % |
| dense (8,192 seeded) | 8 | 2,689 | 5,378 | 336 ns | 0.0051 % |
| dense (8,192 seeded) | 32 | 4,036 | 8,072 | 314 ns | 0.0025 % |
| member down (real marks) | 1 | 1,121 | 2,242 | 195 ns | 0.020 % |
| member down (real marks) | 8 | 3,232 | 6,464 | 257 ns | 0.0056 % |
| member down (real marks) | 32 | 4,719 | 9,438 | 217 ns | 0.0022 % |

Observations:

- The empty-table atomic fast path already avoids every acquisition at any
  concurrency (dg_locks=0 at 1/8/32).
- With any entries, the cost is exactly ~2 acquisitions per REPORT record
  (flags + drop) at ~200–340 ns each — and the per-acquisition cost does
  **not** grow from 1 to 32 writers (296→312 ns): hold times are tens of ns,
  so futex sleeps essentially never happen; there is no contention to
  amortize. (The ~300 ns includes the instrumentation's own two
  `clock_gettime` calls; the true lock cost is lower.)
- Probe cost does not grow with table density (8,192-entry table: same
  per-acquisition cost) — open addressing at ≤50 % load.
- Real degraded marks (member-down column, `dg_notes` = fsync count: every
  write degraded, noted, reported, dropped) show the same cost as seeded
  entries.
- REPORT wall time is dominated by the server side (per-record Raft commit);
  client lock wait is noise within it.

## ORCD confirmation stage (node9901, real hardware, TCP)

Same driver and instrumentation on node9901 (single-host loopback cluster
under `/tmp`, TCP), 8/32 writers, all four states: identical shape — zero
acquisitions empty, ~180–210 ns per acquisition with entries, lock-wait
share ≤ 0.023 % at 32 writers (seeded-1: 0.0106 %, seeded-8192: 0.0116 %,
member-down: 0.0118 %; raw:
node9901 `/data1/efs/evidence/taskAM-20261029/orcd-matrix-summary.json`).
Throughput was ~5× intel1's; the conclusion is unchanged. RDMA is not a
separate column for AM: the lock is client-process-local, taken before any
byte reaches a transport, so the transport cannot change the numerator; the
REPORT wall denominator only grows on a slower transport, shrinking the
share further.

## Gates

Named AM gates on the clean (uninstrumented) build at `9c4bfd171` on intel1,
all PASS: `tests/test_k35_fsync_degraded.py`,
`tests/test_report_partial_verdict.py`, `tests/test_report_resolved_retry.py`,
`tests/test_publication_session`, `tests/test_publication_session_rpc.py`,
`tests/test_publication_session_reuse.py`, `tests/test_namespace_session`.
A new deterministic note/drop/publication interleaving test is **not
written**: there is no new code to pin, and the existing interleaving gates
above already cover the unchanged semantics. TSan on the client tests runs
inside test-all (`tsan-check`, sanitizer entries).

Full gate: `make test-all` on intel1 at `9c4bfd171` (the tree carries only
the two measurement scripts beyond devel): **TOTAL 546 FAIL 1 NOTRUN 12
(822s), OVERBUDGET 10**; the FAIL is the pre-existing b6 slow-mode defect
above, the NOTRUN are the host limits listed in the header. Raw:
intel1 `~/efs-artifacts/taskAM-20261029/test-all-summary.txt`.

## Reproduction

```
patch -p1 -R < scripts/evidence/am/am-instrument.patch   # no-op check: tree must be clean
patch -p1    < scripts/evidence/am/am-instrument.patch   # apply TEMP-AM instrumentation
make -j8 efsd efs-fuse
python3 scripts/evidence/am/am_measure.py --src . --work /tmp/am --writers 1,8,32 --seconds 20
```

Then restore the three touched files (`src/client/write.c`,
`src/client/inode_rpc.c`, `src/client/efs_fuse.c`) before building for any
other purpose. Raw artifacts: intel1 `~/efs-artifacts/taskAM-20261029/`
(matrix and smoke `am-summary.json`, gate logs) with `SHA256SUMS`.
