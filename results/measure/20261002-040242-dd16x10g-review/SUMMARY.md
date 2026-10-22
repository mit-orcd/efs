# 16 × 10 GiB dd, both ends traced — review (Oct 2 2026, 04:02–04:07Z)

## Purpose and linked item

Review of the r749 traced window: one client running 16 concurrent 10 GiB `dd` streams with
recorders on both ends. Primary linked item: **W44 step a** (the idle-hour reading that is D26's
input — what one GC frag pass scans). The review also opened/registered **W48** (4 of 16 streams
stopped early), **D29** (REPORT BUSY-after-commit resend), **W50** (repeating GC record set),
**W51**/**D30** (compaction/follower lag). Added by commit `173e24919`.

## Verdict

PASS as an analysis deliverable (review completed; findings and open questions recorded). No
pass/fail gate attached. Key corrective finding: there is **no per-stream regression and no PUT
amplification** — an earlier reading of the same data as "×14 PUT amplification of one file" was
wrong (the FUSE nodeid census shows 16 inodes).

## Source commit and build identity

Tree `2b5a25df419c` (docs-dirty). Servers rolled at 01:54Z (r749); client binary via the user's
`scripts/client.sh`. No deployed-sha256/identity.json in the run.

## Environment

- Servers fcstor003–006 under `cluster.sh restart --clients --perf --strace`; client fstor007 under
  `scripts/client.sh --perf --strace` — every absolute MB/s is a tracer number; shapes/ratios are not.
- Workload: 16 × `dd bs=1M` of 10 GiB fresh files on one mount (04:02:43Z–04:07:32Z, 222 s).
- Cluster 19810 context (fcstor003–006 + fstor007); transport not stated in this directory
  (contemporaneous runs on this cluster were RDMA — inference, not recorded here).

## Method

Reductions of raw captures held outside the repo (`~/orcd/scratch/efs/perf/efs-mount/` and
`.../perf/cluster-20261002-0423/`): FUSE request census per nodeid (`client/ana-fuse-writers.txt`),
per-thread strace, dd write() latency histogram; per-server perf flat, dd-window syscall tables,
fragment creates/s, GC/REPORT/compaction log analysis, raft log tail, unlinks per 10 min. No gate
script; analysis only.

## Results

| Metric | Value |
| --- | --- |
| Aggregate through FUSE | 140 433 MiB in 222 s = 663 MB/s (16 inodes, 12 full + 4 stopped at 3.6–5.2 GiB) |
| Traced stream (dat16) | 10 GiB / 213 s = 50 MB/s + 74.6 s close() = 37.3 MB/s |
| Fragment creates per server | flat 3700–4000/s (= 16 streams × 50 MB/s × 3/4; no amplification) |
| Publish path | REPORT every ~1.7 s; pack 100–133 µs/rec under l0=100–170; snapshots grew 8 k→570 k records; publish ~2.7 k rec/s vs PUT ~5 k chunks/s → backlog lands on the last close |
| REPORT BUSY-after-commit | every REPORT but the last paid pack twice (`fail=wait/-13` then `skip=all` resend to the other dual host) — D29's motivation |
| apply-sleep | ×31 fcstor004, ×103 fcstor005 in the window (compaction storm, L0+L1 merges 3–5 s) |
| Idle GC pass (W44 a) | leaders walk ~1 M tombstones to emit 257 live records every ~1.6 s (fcstor003: 5534 passes ≈ 38 % of a core); raft tail 99.7 % GC_ACK, 5–6 commits/s per group |
| Client CPU | ~0.9 core for 663 MB/s (blake3 19 %, memmove 10.7 %) — CPU not the wall |

## Limitations and remaining gates

- Tracer on both ends; absolute rates are depressed (compare the untraced Oct 1 20:56Z 16× run at
  ~670–700 MB/s).
- Open questions handed off: Q1=W48 (4 early-stopped streams, cause not in this data), Q2=D29
  (RECEIPT for BUSY-after-commit — ask), Q3 (publish < PUT throughput; re-measure untraced after
  D26), Q4=W50 (record GC identities), Q5=W51/D30 (classify apply-sleep episodes).
- Harness outputs for 15 of the 16 streams were not in the perf dir (only dat16's).

## Evidence index

| file | sha256 (16) |
| --- | --- |
| SUMMARY.txt (full review incl. open questions) | 9dc09073ce45ba38 |
| client/ana-fuse-writers.txt (FUSE nodeid census — 16 streams) | 59a1324081aa14dd |
| client/dd.trace-reduced.txt (traced-stream write latency) | 69a4ffc3537b0174 |
| client/flat.txt (client perf flat profile) | ecc3445af7cd9bc3 |
| client/efs-fuse-efs-mount.log (report-landed/slow-ok/report-stale lines) | 27051d31923681f6 |
| fcstor004/ana-log.txt (report-split, apply-sleep, kv-compact) | 9c7340471233c520 |
| fcstor003/ana-log.txt (g0 leader GC-pass analysis) | cafed626bbf86a1a |
| fcstor006/ana-log.txt (g2 leader GC-pass analysis) | 482dbe1bda1933f8 |
| fcstor003/ana-dd-frag-per-sec.txt (fragment creates/s) | 399bb8cba74d1658 |
| fcstor004/ana-dd-syscalls.txt (server dd-window syscall table) | 070391b518fa321b |
| fcstor003/ana-raft-tail.txt (raft log tail: 99.7 % GC_ACK) | a5eb3a03b65b5bb9 |
