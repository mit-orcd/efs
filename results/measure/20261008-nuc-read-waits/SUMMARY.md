# NUC ReadIndex read-wait investigation — Oct 8, 2026

## Purpose and linked item

Supports **W66** (metadata-group performance follow-up). Interpretation and the
next gate live in `docs/status/metadata-group-perf-followup-20261008.md`. This
is a read-only live investigation of why one metadata Raft group answers reads
slower than the other on the NUC cluster; no gate verdict was sought.

## Verdict

**PASS** as executed measurement — all 48 management-CLI attribute probes
returned rc=0 and the before/after Raft status was captured. This is
observational evidence of a group-0 vs group-2 read-latency split, not a
performance gate and not proof of a daemon defect.

## Source commit and build identity

Installed build `devel-find-stats` commit **befaee95** (per README). No
deployed-version.txt/identity.json in the run; binary hashes from the earlier
capture remain with the external capture (see Limitations).

## Environment

- NUC cluster, four daemons (127.0.0.1:17432–17435), TCP, existing live
  workload active with unequal group write pressure.
- Two metadata Raft groups: group 0 (voters nodes 1–3), group 2 (voters
  nodes 2–4). Probed existing inodes 11434383 (group 0) and 1946256 (group 2).

## Method

- Runner: `probe.py` (preserved in this directory). 12 repeats × 2 inodes ×
  2 endpoints (ports 17432/17433), alternating group order, via
  `efs-mgmt raft-getattr`; before/after `efs-mgmt raft-status` on all four
  nodes. A preliminary runner failed to serialize its after-status and was
  discarded; only the successful rerun is retained (`probes.json`).
- A separate ~30 s all-process strace capture (`node*-window.log`,
  `summary.json`) predates the untraced probes.
- No daemon restart, code deployment, leadership or durability change.

## Results

End-to-end probe latency (includes remote CLI launch, socket setup, request
processing), 12 samples per cell, all rc=0:

| Cell | Median | p95 | Max |
|---|---:|---:|---:|
| group 0 via 17432 | 1.6 ms | 2.1 ms | 2.1 ms |
| group 0 via 17433 | 1.4 ms | 2.0 ms | 2.0 ms |
| group 2 via 17432 | 16.5 ms | 29.5 ms | 29.5 ms |
| group 2 via 17433 | 18.7 ms | 37.4 ms | 37.4 ms |

## Limitations and remaining gates

- One inode per group, 12 samples per cell: no capacity or controlled hardware
  comparison. Times are end-to-end, not isolated server handler times.
- Strace capture and probe dataset are from different moments; the full strace
  capture remains externally at `/data1/efs/evidence/bench/tiny-strace-20261008T2100a`.
- Source excerpts were read from the installed tree; they are not a
  source-to-binary identity claim.
- W66 remains open; next gates are listed in the linked status page
  (slow durability/pump phases, per-round diagnosis, scaling gates).

## Evidence index

| sha256 (16) | File |
|---|---|
| 3ba5148bc4c7bc25 | `probes.json` — 48 timed probes + before/after status |
| db6460d1560a5320 | `probe.py` — exact runner |
| bccb564194c6572d | `summary.json` — earlier 30 s strace summary |
| b83e847f37dd1318 | `node1-window.log` — strace window, node 1 |
| 641124472a020cac | `source-excerpts.txt` — read-path code excerpts |
| 7be953c18ee0148e | `README.md` — original run notes |
