# SUMMARY — results/measure/20260930-040600-postfix-review

Post-fix review of the user's strace runs (ls/find/du/rsync/ecrawl/ecopy) on
the `06916bc7e5c1-dirty` + uncommitted raft.c wedge-fix build — verdict: the
remaining ecopy "verification metadata mismatch" is NOT the transport fix's
domain.

## Purpose and linked item

- Review gate after the raft wedge fix (F1/F2: RDMA peer-lane timeout +
  inbox-drop counter): do the user's ecopy mismatch lines persist? Answer:
  yes — it is a metadata-semantics bug (utimens between last write and close
  lost; mtime becomes close time), analyzed in `20260930-044100-mtime-utimens-close`.

## Verdict

**PASS** (review completed; defect re-scoped): transport fixes verified not to
address the ecopy mismatch; the mismatch reproduced on an idle cluster from one
client → metadata write-path bug.

## Source commit and build identity

Cluster `06916bc7e5c1-dirty` (HEAD 06916bc7 + uncommitted raft.c wedge fix),
rolled --all 03:56Z with --perf only, `EFS_TRANSPORT=rdma EFS_RAFT_OBS=1`;
client fstor007 `client.sh --perf` (pid 378525, RDMA, FUSE_OK).

## Environment

fcstor003–006 servers, single client fstor007, RDMA; no other client.

## Method

Reviewed `~/orcd/scratch/efs/perf/efs-mount/{ls,find,du,rsync,ecrawl,ecopy}.strace.txt`
(04:01–04:05Z content) plus perf flat/by_thread/callers from the 00:05:24 perf
stop; reduction script `strace_reduce.py` (in this dir); raw pulls alongside.

## Results

F1/F2 = raft transport fixes only; ecopy's 153 "verification metadata mismatch"
lines at 04:03:51Z persist → re-scoped to the mtime/utimens write-path bug (see
the mtime-utimens-close run).

## Limitations and remaining gates

The mtime-utimens-close run carries the reproducer and gate.

## Evidence index

- `SUMMARY.txt` — review + verdict (run `shasum -a 256 SUMMARY.txt | cut -c1-16`
  here)
- `client-log-and-ecopy.txt`, `client-perf-by_thread.txt`,
  `client-perf-callers-top.txt`, `client-perf-flat.txt`
