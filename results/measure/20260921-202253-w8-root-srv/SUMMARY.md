# 20260921-202253-w8-root-srv — server-side strace of slow root mkdir/rmdir

## Purpose and linked item
W8 (9-node POSIX suite 1) diagnostic: explains the 0.7–1.4 s root mkdir/rmdir
stalls and the "rmdir ENOENT on a dir I just made" symptom from
`results/measure/20260921-194332-w8-root`. Server-side strace of all four
efsd during 12 client mkdir+rmdir pairs. Adding commit: `165e77969` ("txn:
resolve/drop scan the three txn kinds, not the whole shard"). Companion perf
run: `20260921-202715-w8-root-perf` (referenced, not in this batch).

## Verdict
**PASS** (diagnostic run — no pass/fail gate). Diagnosis achieved: the stall is on-CPU
inside the raft apply under `h->mu`, not disk and not network.

## Source commit and build identity
`b2184a5c7faf-dirty` on fcstor003–006 (preflight.txt, 2026-09-21T20:22:53Z).

## Environment
Cluster 19810: efsd on fcstor003 (g0 leader), fcstor004 (dual host, follower
of both groups), fcstor005, fcstor006 (g2 leader); g0 voters 0x7, g2 voters
0xe. Client fcstor007 on FUSE /tmp/efs-mount, TCP transport; client talked to
172.16.223.57 (fcstor003) and .58 (fcstor004) only (client-peers.txt).

## Method
Script: `tests/measure/w8_root_srv.sh`. Strace attached to all four efsd
processes while fcstor007 issued 12 mkdir+rmdir pairs at the mount root;
per-op client timings recorded (client-ops.txt). 10 of 24 ops were slow
(0.74–1.44 s; the rest 5–65 ms).

## Results
- fcstor004 (follower of both groups): 3–5 threads released from one futex
  wait at the same instant, 0.70/1.05 s after starting — waiters on `h->mu` /
  `applied_cv` released when the pump finished one apply.
- fcstor005: one futex wait ETIMEDOUT at exactly 0.400 s = the
  `host_wait_applied`/`host_read_index` deadline → EFS_ERR_BUSY to the
  caller (the BUSY the client retries on).
- fcstor003/fcstor006 (leaders): 0.7–1.1 s recvfrom gaps on peer conns — no
  raft traffic while the apply was stalled.
- Recurring quanta: 0.40 s (deadline), ~0.70 s, ~1.05 s. No syscall in the
  trace is disk (no fdatasync/pwrite ≥ 0.15 s) → on-CPU stall in the apply.

Consequence chain recorded: slow RESOLVE apply under `h->mu` → 400 ms
deadline → BUSY → client backoff/retry (up to 16 attempts = 10.4 s) → a
retried UNLINK whose first attempt committed returns NOT_FOUND → the "rmdir
ENOENT" symptom.

## Limitations and remaining gates
Root cause inside the apply (whole-shard txn scans) fixed in `165e779`;
remaining fresh-parent hang tracked by `20260921-203405-w8-root`. Strace adds
observer overhead; timings are indicative, not clean benchmarks.

## Evidence index
- `4bda9ad8923b81fd` SUMMARY.txt — full diagnosis and consequence chain
- `1809c74fdcbc8cb5` client-ops.txt — per-op client timings (24 ops)
- `2416d749f5243765` srv-fcstor004.txt — strace: simultaneous futex releases
- `73ad227dd9a48661` srv-fcstor005.txt — strace: 0.400 s ETIMEDOUT deadline
- `486064742e6b0917` srv-fcstor003.txt — strace: leader peer-conn gaps
- `5c724532f181b5ee` preflight.txt — build identity and raft state
