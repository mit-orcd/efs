# SUMMARY — results/io500/20260929-123635-rdma

Debug IO-500 9×4 (36 ranks, fcstor007–015), RDMA, stonewall 1 s. ior-hard-write
failed with 31 fsync failures; one client's fuse hung until killed.

## Purpose and linked item

- Same-day follow-up of the 20260928-150609 RDMA debug run after rolling build
  `a53b253f2455-dirty` (8h span skip + D12 file-cap backstop, rolled 12:29Z
  without --perf/--strace). Added by commit `2dd77dac8` (L0-fold compaction).

## Verdict

**FAIL**: `ior-easy-write` (2.218 GiB/s) and `mdtest-easy-write` (1.765 kIOPS)
finished [INVALID-by-stonewall], but `ior-hard-write` (shared file, ino
2595379) printed **31 fsync failures and no bandwidth**. fcstor012's efs-fuse
went uninterruptible-D and ignored SIGKILL until killed at 12:50Z.

## Source commit and build identity

Servers/clients: `a53b253f2455-dirty` (recorded in NOTE.txt).

## Environment

9 hosts fcstor007–015, 4 ranks each, `tests/perf/io500/config-debug.ini`,
stonewall 1 s, `EFS_TRANSPORT=rdma`. Launched 12:36:34Z; log `last-run.log`.

## Method

`io500` standard driver, `config-debug.ini`. Failure forensics in NOTE.txt:
fcstor007 `report-loop rounds=1 stale=1 rc=-14` then efs_rc=-1; fcstor012 BUSY
(rc=-13, 9.0 s) then STALE (rc=-14, 23.5 s) on the same inode, then ll_fsync
stuck in-kernel — the client reclaimer held the shard I/O lock across
`efs_client_fetch_published_chunk` of chunk 170279, with two fragment GETs in
`efs_rdma_recv_wait` (30 s timeout) on nodes 3/4.

## Results

| phase | result |
|---|---|
| ior-easy-write | 2.218 GiB/s, 19.7 s [INVALID] |
| mdtest-easy-write | 1.765 kIOPS, 2.7 s [INVALID] |
| ior-hard-write | 31 fsync failures, no bandwidth |

fcstor004 during the run: report-split 23 rc=0, 73 rc=-13, 3 rc=-14; kv-compact
L0 7→3 (L1 stayed 397); 1582 apply-sleep lines; D12 file cap held; the 400 ms
apply wait still returned BUSY.

## Limitations and remaining gates

Shard-lock-across-RDMA-fetch stall documented here; later builds (20260930+)
complete this phase cleanly — see those summaries.

## Evidence index

- `NOTE.txt` — 1d0da4161db9f065 (failure forensics)
- `result_summary.txt` — 48558fd626951e37 (partial phases)
- `last-run.log` — e66afdbd52ac5e1e
