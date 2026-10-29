# AP — packed segment GET: one aligned read for payload and tail (2026-10-28)

Coordinator report: branch `agent/devel-ap`, head `9c4bfd171ccc5450ae852a2597351150e543730a`
(docs-only branch; measurement-only task, no product change). Gate on intel2:
`TOTAL 546 FAIL 1 NOTRUN 11 STATUS FAIL (719s) OVERBUDGET 10` — the FAIL is the
untouched-base K55 slow-mode fsync EIO, identical signature on every gate of
this commit. Evidence: this page; raw data intel2
`~/efs-artifacts/taskAP-20261028/`. NOTRUN: the node9901 cold-read column —
ORCD login requires keyboard-interactive credentials, which this agent must
not type (access was requested from the user; not provided during the run).
Product defects found: none from this task's mechanism (the gate's K55 base
failure is unrelated and already registered).

**Verdict: PASS — measured, no change.** The two-read mechanism is confirmed
and priced: a combined read would save exactly one pread and one aligned
bounce per packed GET (~1–2 preads per small-file read out of 6–11 measured),
but no end-to-end number leaves the spread — cold and repeated packed-read
p95/p99 are identical within rep-to-rep spread on every configuration, and
that spread is 2–4× wider than the entire candidate saving.

## Purpose and checklist item

Task AP of the AP–AR server disk I/O block
(`docs/status/alpha-1.0-2plus1-checklist.md`, AP checkbox):
`src/server/segment_store.c::server_segment_read` calls `sg_read_tail` and
then `server_frag_pread` for the payload — two O_DIRECT reads of adjacent
bytes, each with its own 4 KiB-aligned bounce (`src/server/store.c:63-83`).
Ordinary `read_file_bytes` already combines them. The candidate saving is a
submission and a bounce per packed GET, not device bytes.

## Tested commit and build identity

- Source: devel `9c4bfd171ccc5450ae852a2597351150e543730a`, branch
  `agent/devel-ap` (no product commit).
- Measurement build: clean devel on intel2 (8 CPUs, NVMe `nvme0n1`, O_DIRECT
  throughout), plain `make` + `make tests/faults/efsd`; gate build
  `efsd 9c4bfd171... (branch agent/devel-ap, built 2026-10-29 on intel2)`.

## Method

Per run: a fresh 3-member loopback TCP cluster (packing on a short age),
2000 × 4 KiB files written through one mount and packed into segments, then
two passes over all files on two fresh reader mounts (cold client each) —
"cold" and "repeated" passes. Loads: 1 and 8 readers, 3 reps. Two variants:
`traced` (node 1's efsd under `strace -c -f` for both passes; per-syscall
counts) and `clean` (no strace; latency reference). Device requests are
`/proc/diskstats` deltas for the store's NVMe device. No product patch was
needed anywhere; `ap_measure.py`/`ap_run.sh` are retained in the artifacts.

## Results

Clean runs, per-file read latency (open/read/close), medians of 3 reps —
spread shown for p95:

| readers | pass | p50 | p95 (spread) | p99 (spread) |
|---|---|---|---|---|
| 1 | cold | 4.89–7.05 ms | 7.77–10.19 ms | 11.38–13.47 ms |
| 1 | repeated | 5.72–6.99 ms | 8.47–10.07 ms | 11.09–13.49 ms |
| 8 | cold | 8.23–10.32 ms | 12.96–25.41 ms | 18.04–40.24 ms |
| 8 | repeated | 7.63–10.51 ms | 12.69–17.32 ms | 15.70–21.36 ms |

Cold and repeated are indistinguishable: with O_DIRECT there is no server
page-cache reuse, and each pass uses a fresh client mount, so the second pass
buys nothing — both measure the same packed-GET path.

Traced runs (both passes = 4,000 file reads):

| config | pread64 total | pread64 per file | device reads per file |
|---|---|---|---|
| 1 reader (3 reps) | 24,215 / 45,330 / 45,500 | 6.1 / 11.3 / 11.4 | 4.0 / 4.0 / 4.0 |
| 8 readers (3 reps) | 24,118 / 37,762 / 23,430 | 6.0 / 9.4 / 5.9 | 4.8 / 4.3 / 4.6 |

The segment-GET share is 2 preads per GET (tail + payload, from the code);
the remaining 4–9 preads per file are metadata/KV/Raft I/O and vary with
background work, as do total device reads (~4 per file). A combined read
would remove one of those two preads and one aligned bounce per GET — at
1–2 GETs per file, that is 1–2 preads and 1–2 small (4 KiB) device requests
per file read, bounded by the measured totals.

strace's own overhead is visible in the traced latencies (cold p99 at 1
reader reached 57 ms once, against 11–13 ms untraced); traced runs were used
only for syscall/device counts, clean runs for latency.

## Why no change

- The candidate saving per packed GET is one pread and one bounce: tens of
  microseconds at most on this NVMe, against per-file p95 of 8–25 ms whose
  rep-to-rep spread is 2–4× larger. Nothing measurable moves outside the
  spread; cold and repeated passes are the same measurement, not two regimes.
- Device bytes are unchanged by construction (both parts are read either
  way), and the device-request saving is the small tail read only — it does
  not show against the metadata/KV background variance even at 3 reps.
- Per the AP–AR family rule (a change needs a number that leaves the spread:
  device requests, bytes, syscalls, fsync count, p95/p99 on real storage),
  the combined read does not qualify. It remains a possible small cleanup,
  not a measured optimization.

## Gate

`make test-all` on intel2 via `scripts/make_test_summary.sh`, 8 jobs, clean
(unmodified devel) tree:

`TOTAL 546 FAIL 1 NOTRUN 11 STATUS FAIL (719s) OVERBUDGET 10 BUILD 31s`

The one FAIL is `test_b6_unavailable.py` slow mode with the identical
`{'F0': ('EIO', 60.06), ...}` signature as every other gate of this commit —
the untouched-base **K55** failure (deterministic 3/3 on unmodified devel,
registered by the AG/AJ gates). NOTRUN 11: the nine no-RDMA-device commands,
`test_cluster_stop.py` and the sanitizer probe: host limits.

## Limitations and NOTRUN

- node9901 cold-read column NOTRUN: ORCD login is keyboard-interactive and
  this agent never types credentials; access was requested and not provided
  during the run. The intel2 NVMe column stands; node9901's disks are the
  remaining follow-up if the user wants the real-host column.
- Segment-GET preads are attributed from the code (2 per GET), not from a
  per-callsite counter; totals above include all daemon preads. No per-callsite
  instrumentation was needed because the end-to-end decision does not turn on
  the exact split.
- No product change, so no new regression test, budgets row or settings
  inventory row; all integrity checks (I25 identity/record/payload hash,
  short/torn-tail refusals) untouched.

## Artifacts

intel2 `~/efs-artifacts/taskAP-20261028/`: `measure/` (12 runs, JSON + logs
including each traced run's full `strace -c` table in `strace_raw`),
`ap_measure.py`, `ap_run.sh`, `matrix.log`, `gate.log`, `summary.txt`,
`timings.tsv`, `PRUNED.txt`, `SHA256SUMS` (manifest sha256:
`a5a9f52aa838572ee13d99ad3dc502375ee78e23390295fd9ffa91957a1b60e2`). Per-run stores and build trees deleted after the runs;
the measurement reproduces from devel `9c4bfd171` plus the retained driver.
