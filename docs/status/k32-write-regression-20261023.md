# K32: ior easy write regression from per-request atomic publication (K14) and per-object tickets (K13) — Oct 23, 2026

**OPEN, alpha 1.0 blocker (performance).** After the fourth-wave merges,
orcd-mini ior easy write fell from about 1.3 GiB/s to about 0.36–0.5 GiB/s;
reads are unchanged. Attribution and cause below; no fix yet.

## Measurement

orcd-mini, freshly formatted per build (three efsd on node9901, RDMA mlx5_0,
direct I/O, 720 GiB quota each; node9900 client), ior easy write only:
8 ranks, `-F -e -w -t 1m -b 2048m` (16 GiB, 16,384 one-MiB writes). Raft
commits read from `efs-mgmt raft-status` on n1 before and after the write.
Driver: `k32_count.sh` (phase deploy plus one ior write; scratch script,
steps listed in Reproduction).

| Build | Content | Write MiB/s | close() s | Raft entries g0 / g2 | Entries per 1 MiB write |
| --- | --- | ---: | ---: | --- | ---: |
| `f47a32754` | devel + W154 | 1,322 | 4.9 | 1,567 / 1,316 | 0.1 |
| `bd2cb0d5a` | + K14 (and K30, W155, D39 model, kind fix) | 661 | 2.8 | 131,462 / 194,084 | 19.8 |
| `757d76844` | + K13 (head) | 359 | 8.9 | 202,813 / 266,065 | 28.6 |

Earlier fresh-format runs agree: `24e58fb1d` (before the wave) 1,242 mean;
pre-K13 three runs 749 / 632 / 661; head 509 / 492 / 483 and 527 / 481 /
463. Reads stay at 4.3–6.2 GiB/s on every build.

## Cause

- **Before K14** the client's REPORT batched the publications of many writes
  into few Raft entries (0.1 per write).
- **K14** (atomic multi-chunk publication, `d2369a49d`) makes every FUSE
  write request of two or more chunks one atomic unit, and every such unit
  spans both metadata groups by design (adjacent chunks alternate groups), so
  each 1 MiB write becomes its own cross-group transaction: PREPARE in both
  groups, DECIDE, RESOLVE in both, RETIRE — about 20 Raft entries per write,
  each a quorum round trip.
- **K13** (ticketed PUT/REPORT, `1f3b25997`) adds about 9 entries per write:
  per-object ticket admission before the first fragment (eight 128 KiB
  objects per 1 MiB write) and the owner's settles.
- **Latency-bound, not CPU-bound:** server profiles during the head write are
  dominated by the scheduler's idle balancing and futex waits; n3's top
  user symbol is the RDMA receive copy at about 4%. The write is gated by
  serialized consensus round trips.

## Direction for the fix (to be implemented and gated)

- K14: keep the atomicity contract (a unit is all or nothing; a reader sees
  one state) but publish many units in one transaction: a superset published
  atomically still satisfies each unit's contract. Batch all pending units of
  a REPORT cycle per file (or per group pair) into one cross-group
  transaction; keep single-group units as one entry; avoid RETIRE per unit
  (batch or piggyback). Target: entries per 1 MiB write back near the
  pre-K14 0.1–1 range.
- K13: batch ticket admissions (one ADMIT for all objects of a write or a
  REPORT cycle) and settles.
- Gate: this table rerun; ior easy write at or above the `24e58fb1d` level
  (about 1.2 GiB/s) with the K14 torn-read gate and the K13 ticket gate still
  passing.

## Reproduction

For each build: build release dirs on node9901 and node9900 from
`git archive <commit>` (same `EFS_GIT_ID`, private libfuse); stop clients
(after `fuser -m` checks) and daemons, `edelete` stores n1–n3 and
`.mkfs-done`, switch both `src` symlinks, `EFS_CLUSTER=orcd-mini
EFS_START_TIMEOUT=900 ./start.sh` (fresh mkfs), mount node9900; read Raft
commits; run the ior write above; read Raft commits again.

## Raw evidence (outside Git)

- node9901 `/data1/efs/traces/k32-20261023/`: perf call graphs of n1–n3
  during a head write (`perf-n<i>.data`, 499 Hz, 45 s), Raft status before
  and after; manifest `SHA256SUMS` (sha256 `f25d65ca…700c`).
- node9900 `/data1/efs/orcd-mini-client/prof/k32/`: client perf during the
  same write, ior output; manifest `SHA256SUMS` (sha256 `fcade7f4…d1e4`).
- node9900 `/data1/efs/orcd-mini-client/prof/k27/{deploy-757d76844,
  ab-preK13-bd2cb0d5a,ab-W154only-f47a32754,ab-head2-757d76844}/` ior easy
  outputs.

## Limitations

One cluster, one write per build for the entry counts (three for the
throughput table rows from the phase runs). The per-step entry breakdown
(how many of the 20 are PREPARE, DECIDE, RESOLVE, RETIRE) is inferred from
the K14 design, not counted per command type.
