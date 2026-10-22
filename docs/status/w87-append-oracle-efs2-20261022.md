# W87 / K9 · concurrent O_APPEND oracle on efs2 — INCOMPLETE, not reproduced (W87 OPEN)

**Purpose.** Reproduce W87 (buffered concurrent append loses records under
load; [work item](../backlog/work-items.md#w87)), alpha 1.0 register K9.
**Verdict: INCOMPLETE. The loss did not reproduce and no root cause was found.** No acknowledged record was
lost, duplicated, torn or reordered in any completed round. No cause was
identified and no source fix was made. W87 stays OPEN.

## Tested build

- Source: devel `7c510c60f` (plus the oracle scripts below, which do not
  touch the daemon or client). Built on efs2 with `make -j4`, gcc, aarch64,
  `EFS_BUILD_ID=unknown` (source tree from `git archive`).
- The original failure (Oct 7 NUC GC fixture `gc-buffered-3ohdte4d`) predates
  roughly 800 client/server/meta commits, including durable append receipts,
  per-inode reserve/reply ownership (the identical-RPC race fix), atomic
  single-shard reservation and completion, and append burst publication.

## Environment

efs2: Fedora 42 VM, 4 vCPU, ~1.9 GB RAM, one virtio data disk (`/data1`).
Private cluster: three efsd on 127.0.0.1:18201–18203, `EFS_MD_RAFT_N=3`,
TCP, server page cache (`--no-direct-io`, the GC fixture's "buffered" mode),
4 GiB quota, stores under `/data1/k09`, one efs-fuse mount. The VM was shared
with other sessions; load average 18–27 and ~55% iowait throughout. Physical
GC was active (default).

## Method

`tests/live/append_oracle.py`: N writer processes append uniquely numbered,
variable-length records to one file with O_APPEND (persistent fd, open/close
per record, or mixed; optional fsync cadence). After all writers fsync/close,
the file is read warm, then cold after a remount or a full three-daemon
restart. Every acknowledged record must appear exactly once, intact, with each
writer's records in issue order, and the byte count must match. A write that
returned an error may be absent, present once, or a zero hole; it never
excuses a lost acknowledged record. Missing records are reported with their
present neighbours' offsets. `tests/test_append_oracle.py` (in `make test`)
checks this classification without a cluster.

`tests/live/append_oracle_cluster.sh start|rounds|remount|restart|stop`
starts the cluster and runs rounds. Example:

```
tests/live/append_oracle_cluster.sh start REL /data1/k09 18200
tests/live/append_oracle_cluster.sh rounds REL /data1/k09 18200 5 -- --writers 4 --records 60 --mode mixed --fsync-every 7
tests/live/append_oracle_cluster.sh stop REL /data1/k09 18200   # then delete /data1/k09
```

Background load in later phases (drivers were scratch scripts, not committed):
a 4 MiB client cache (`EFS_DCACHE_BYTES=4194304`), a churn writer creating and
deleting 128 KiB–1 MiB files on the same mount (drives physical GC), and either
the original `posix_suite.py --filter concurrent_appends` cases in a loop or
the full core suite with `--jobs 4`. Churn/POSIX were paused for each cold step.

## Results

| Phase | Rounds | Shape | Acked records | Result |
| --- | --- | --- | --- | --- |
| no extra load | 1 | 8 writers × 200, mixed | 1600 | exact warm/cold |
| no extra load | 1 | 16 × 250, open/close per record (1094 s) | 4000 | exact warm/cold |
| 4 MiB cache + churn | 9 | 8×40 per-record / 12×60 mixed fsync 7 / 16×60 persistent | 5998 | exact; 2 writes EBUSY, no bytes |
| + POSIX concurrent_appends loop | 9 | same rotation | 6000 | exact; POSIX loop 26 iterations, 2/2 pass each |
| + full core POSIX `--jobs 4` | 3 | same rotation | 1999 | exact; 1 write EBUSY; POSIX 3 iterations rc 0 |

23 completed rounds, 19,597 acknowledged records, 5 of them with a cold
three-daemon restart. Two longer 16-writer, 3000-byte-record rounds were
abandoned for VM throughput (~2 KB/s) and are not counted.

**Observations (not W87).** Under this load the 8-second REPORT budget expired
and three O_APPEND `write(2)` calls returned EBUSY before reserving (no bytes
written, file exact). `unlink(2)` on the churn files also returned EBUSY when
the server was busy. Both are application-visible transient errors under load.

## Limitations and remaining gate

- One shared, saturated VM, TCP loopback, three daemons, one mount. No NUC,
  no four-node GC fixture, no two-mount peer append, no RDMA, no direct I/O.
- An external cleanup by another session stopped the cluster once mid-round;
  that round is excluded. Later live runs on efs-VMs were stopped by user
  direction (efs1–3 are for compile and `make test` only).
- Live reproduction is not sub-session work (user direction, Oct 22); the
  bounded rerun is NOTRUN. Intended parameters, 4 writers, ≤10 minutes each: `--mode perrecord --records 60`, `--mode mixed --fsync-every 7`,
  `--mode persist --pad-max 3000`, each with remount and restart cold steps,
  then with `EFS_DCACHE_BYTES=4194304` and churn.
- W87's gate is unchanged: repeated original-load buffered acceptance with
  exact records, a retained failing run if one occurs, and a focused
  regression for its cause. Passing rounds here do not close it.

Raw round JSON and logs were on efs2 under `/data1/k09/logs` and were deleted
with the fixture; this summary is the retained record.
