# W51 — what follower apply lag is made of — Oct 2 2026

## Purpose and linked item

W51 ("what the follower apply lag is made of", `docs/backlog/work-items.md`
"W51 · what the follower apply lag is made of — CLOSED (plan row 14)"; plan
item P2.4). Question: per `apply-sleep` episode, was the pump blocked on
`l->mu` / the compactor, inside `efs_meta_apply_*`, or idle waiting for
AppendEntries? Feeds the D30 decision. Run added by commit `33b04eec8`.
Original write-up: `SUMMARY.txt` in this directory.

## Verdict

**PASS** (classification completed): of 144 `apply-sleep` episodes, zero were
transport/AE-wait (`idx-applied` never ≥ 8). The populated classes are
compact-overlap and small-gap apply. **D30 remains an ask** — this table does
not decide it.

## Source commit and build identity

No new code deployed; this is a re-classification of existing traces. The
underlying traces were captured on the Oct 2 `start760` / P0.2 runs (see
`results/measure/20261002-054132-p0-x16` and
`results/measure/20261002-060052-p1-d23-w41`). No build identity is recorded
in this run directory itself.

## Environment

Cluster 19810 (fcstor003–006 servers), traces from the 16 × 10 GiB dd P0.2
workload (05:41Z) and the P1 gate passes. No new cluster activity for this
run — analysis only.

## Method

Classifier `~/efs-runs/w51-classify.py` (outside the repo) over
`results/measure/20261002-054132-p0-x16/apply-sleep-compact-gc.txt`.
Classes: an apply-sleep overlapping a `kv-compact: start/end` window on the
same host → compact-overlap; within 50 ms of a window edge →
compact-adjacent; `idx-applied >= 8` → lag-gap; `us >= 100000` → long-wait;
else small-gap-no-compact. No new `--strace` capture was taken.

## Results

Classification of the 144 apply-sleep lines (from `SUMMARY.txt`):

| class | n | reading |
| --- | ---: | --- |
| compact-overlap | 47 | sleep inside a `kv-compact` window on that host |
| compact-adjacent | 7 | within 50 ms of a compact window edge |
| small-gap-no-compact | 89 | `idx-applied` 1–3, 20–36 ms, no compact line |
| long-wait | 1 | one 101 ms sample (fcstor004) |
| lag-gap | 0 | no episode with `idx-applied >= 8` |

us min=20066, p50=28484, max=101412. idx-applied min=1, p50=1, max=3.
By host: fcstor004 95, fcstor003 32, fcstor006 17, fcstor005 0.

Related non-parsed notes: fcstor006 `apply_max` 100–160 ms during 8192-record
REPORT applies (slow application of publish batches); the 12:54Z `l0=143`
compaction storm is compact-overlap on the write path's REPORT wait.

## Limitations and remaining gates

- Cannot separate "pump in `kv_compact`/`l->mu`" from "pump in
  `efs_meta_apply_publish`" for the 89 small-gap episodes; that needs a
  follower `--perf --strace` during a 16× write, which was not run.
- D30 (what to change) is explicitly not decided by this table.
- Raw input trace lives in another run directory
  (`results/measure/20261002-054132-p0-x16/`); this directory holds only the
  write-up.

## Evidence index

- `SUMMARY.txt` — e41509d72e53a68a — complete classification write-up and tables

(The directory contains no other files; the classified raw trace is
`../20261002-054132-p0-x16/apply-sleep-compact-gc.txt`.)
