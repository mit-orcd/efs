# Server perf capture, near-idle cluster + one 10 GiB dd — Oct 2 2026 19:22–19:52Z

## Purpose and linked item

Whole-window server CPU profile of cluster 19810 under minimal load (one
user dd), to identify where server CPU goes on the PUT path, follower
apply/compaction shape, and leader GC drain rate. Feeds D26 (GC shape),
D9/D10 (compaction), the W13 follow-on (L0-per-shard flush), and the
next-performance-package hint (P3/P4). Added by commit `ab4922cb0`; context
in `docs/archive/project-history.md` and the D26 entry in
`docs/backlog/work-items.md`. Original write-up: `SUMMARY.txt`.

## Verdict

**PASS** as a capture (1764 s window, all four `efsd.data` footers OK, no
lost evidence). Findings, not a gate: PUT-path CPU is ~98 % kernel XFS
per-fragment metadata; follower compaction runs ~20 L0 flushes/s during the
dd; leader GC drains at ≤ ~400 records/s/group. The open question — whether
the identical `gc-pass` counters hide failing deletes — was **not decided**
here (needs a GC-prefix key count on a KV copy or a W50-style EFS_GC_DBG run
with the cursor).

## Source commit and build identity

Servers: `efc0f499cbae-dirty` (header of `SUMMARY.txt`), started with
`cluster.sh start --perf` (perf record -g -F 999 attached from efsd start).

## Environment

Cluster 19810: servers fcstor003–006, RDMA, `--perf`. Clients fcstor003–015
down the whole window; only load was the user's fstor007 client running one
`dd if=/data1/… of=/tmp/efs-mount/001/dat16 bs=1M count=10240` (O_TRUNC over
an existing file, no conv=fsync) 19:39:11–19:39:43Z. User-side strace/perf
capture of efs-fuse retained outside Git
(`~/orcd/scratch/efs/perf/efs-mount/`); raw server perf.data in
`~/orcd/scratch/efs/perf/servers-20261002-195156/`.

## Method

`cluster.sh start --perf` 19:22:32Z; idle except the 32 s dd; `cluster.sh
stop` 19:52:16Z SIGTERMs efsd then perf (every capture has a footer).
Reports generated per host: `perf-self-*`, `perf-children-*`, `perf-tid-*`,
`perf-header-*`; full efsd logs `efsd-*.log`; grep digests in
`logs-summary.txt`.

## Results

CPU budget (samples at 999 Hz over 1764 s): fcstor003 ~8.0 %, fcstor004
~10.3 %, fcstor005 ~8.5 %, fcstor006 ~8.8 % of one core — near idle, most of
it in the 32 s dd window.

Inclusive shares (perf-children, 003/004/005/006):

| symbol | 003 | 004 | 005 | 006 | what |
| --- | ---: | ---: | ---: | ---: | --- |
| writer_thread (PUT) | 23.5 % | 20.1 % | 24.4 % | 23.8 % | fragment writes; all kernel XFS (xfs_create 11–12 %, DIO writev + extent alloc 17–21 %) |
| server_handle_conn | 13.0 % | 17.6 % | 9.5 % | 12.7 % | RPC handlers |
| host_pump | 6.9 % | 15.2 % | 16.6 % | 7.6 % | raft apply (publish, lane_sweep, gc_ack) |
| compactor_main | — | 6.9 % | 8.4 % | — | L0 flushes |
| host_gc_thread | 6.7 % | — | — | (same) | leaders only |
| nvme_del_if_sum | 7.6 % | — | — | 7.6 % | GC fragment deletes |
| lsm_batch | — | 8.2 % | 9.4 % | — | KV puts of applies |

Key numbers (from `SUMMARY.txt`):

- dd wrote 10 GiB = 81 920 chunks × 3 fragments / 4 nodes ≈ 61 K fragment
  creates per node in 32 s (~1.9 K/s), each a fresh inode + extent.
- fcstor004: 1215 compactions in the dd minute (~20/s, ~12 MB each), 1290
  over the window, 15.6 GB rewritten, 25 s compactor time; L0 sat at
  260–270 on followers, did not drain while idle. `pub_p50=75901us`.
- apply-sleep on fcstor003: 317 lines, mean 35.5 ms, max 118.5 ms, max gap 6.
- GC: both leaders logged `gc-pass` every ~1.27 s with identical counters
  (`fkeys=516 femit=514 ftomb=2`; 591/594 passes). Structural cause:
  GC_FRAG_BUDGET_US 200 ms stops after the second 256-record scan at ~0.5 ms
  per record (3 serial fragment deletes, 2 remote) → ≤ ~400 records/s/group;
  a 100 GiB overwrite/delete (819 K records) is ≥ 34 min of GC per group.
  With the D26 cursor the tombstone walk is gone (`ftomb=2`).
- Client side (user's capture, fstor007): dd 337 MB/s; efs-fuse profile
  blake3 21.0 %, memmove 14.3 % (7.7 % under efs_rdma_send_frame — W39
  zero-copy engagement to verify), xor_into 10.7 %, FUSE kernel copy 5.9 %.

## Limitations and remaining gates

- Not a baseline: single traced dd, O_TRUNC over an existing file (every PUT
  supersedes a chunk and queues a GC record); strace/perf overhead on the
  client.
- GC queue-vs-stuck ambiguity explicitly left open (see Verdict).
- Compaction shape is D9/D10 (ask); L0-per-shard partitioned flush is the
  unread W13 follow-on.
- Raw perf.data and the user's client capture live outside Git (paths above);
  the reports here are the committed evidence.

## Evidence index

- `SUMMARY.txt` — 85eaa33ae1318a8b — full original analysis (all numbers above)
- `logs-summary.txt` — 91317ec8b2327569 — grep digests of efsd logs
- `perf-children-fcstor003.txt` — ca7772b700ad4133 — inclusive profile, g0 leader
- `perf-children-fcstor004.txt` — 3950308c4d88bc6d — inclusive profile, follower
- `perf-self-fcstor004.txt` — 3af095012e6a1f60 — flat profile, follower
- `perf-header-fcstor003.txt` — 30be620c6627dbaf — capture footer/integrity
- `efsd-fcstor003.log` — 47cf09427fedef07 — full daemon log of the window
- `before.txt` — 7b6b834a59a972b9 — pre-stop cluster state
- `stop.txt` — 6a2170771f8b5bde — cluster.sh stop transcript
