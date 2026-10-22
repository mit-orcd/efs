# orcd-test: why some tiny-files clients run 2× slower — October 22

**Purpose:** user-requested short server profile during the 7 × 10M
tiny-files run, to explain why four clients ran about half as fast as the
other three, and to name the server hot paths.
**Verdict: explained (diagnosis); spread-leader rerun inconclusive (see below).** The gap comes from metadata-group
placement, not from the clients. Each client tree lives in one of the two Raft
groups, chosen by the parity of its top directory's inode. Group 2 carried four
client trees and group 0 three. Each group has a similar throughput ceiling, and
both groups' leaders were on one node. On top of that, a group-2 follower
stopped replicating for 30 minutes after a snapshot-time reconnect.
Not a release gate. The run continues.

| | |
|---|---|
| Source | devel `08cc16af4` (`EFS_GIT_ID=08cc16af4f77`), as in [the suites run](orcd-test-devel-suites-20261022.md) |
| Cluster | fcstor003–006 storage = nodes 1–4, 24 cores each; fcstor007–013 clients; RDMA `mlx5_2`, `.efs` on, direct I/O; `EFS_METADATA_BALANCE` unset (off) |
| Groups | group 0 voters nodes 1,2,3; group 2 voters nodes 2,3,4; **node 2 (fcstor004) led both** |
| Workload | `~/bin/tiny_files.py /tmp/efs-mount/tiny-<host> --max-depth 8 --max-files-per-folder 1000 --max-files 10000000 --workers 16` on each client, started 12:12:27Z (files of 1–512 bytes) |
| Profile | `perf record -F 499 -g -p <efsd>` for 30 s on each storage node at ~12:51Z; thread state/wchan sampling on fcstor004; efsd's own `apply-sleep`/`raft-snap`/`kv-obs` log lines |

## The gap is group placement

`efs_raft_shard_group()` puts odd shards in group 0 and even shards in group 2.
A nested directory keeps its parent's group, so each client's tree stays in one group.

| Client | `tiny-<host>` inode | Group | files/s, 300–600 s | files/s, 2580–2640 s |
|---|---|---|---|---|
| fcstor008, 011, 013 | 29639, 92129, 62139 (odd) | 0 | 593–623 | 318–323 |
| fcstor007, 009, 010, 012 | 61646, 49276, 72990, 52656 (even) | 2 | 369–377 | 292–301 |
| **Group 0 total** (3 clients) | | | **1,814** | 963 |
| **Group 2 total** (4 clients) | | | **1,491** | 1,183 |

At 300–600 s, group 0 ran 1,814 files/s and group 2 ran 1,491. Group 2's
total was also split four ways, which gives the 1.6–1.9× per-client gap. By
44 minutes the group totals had converged and the per-client gap fell to
~1.05×. Each file costs about 4 Raft entries (60 s window: group 0 4,316
entries/s for 1,079 files/s; group 2 5,294 entries/s for 1,296 files/s). Total
throughput fell from ~3,300 files/s (first 10 min) to ~2,300 files/s (40 min).
Rates come from the per-second progress lines in each client's `tiny.log`.

## Group-2 follower stopped for 30 minutes (new finding)

- 12:19:03–05Z: fcstor005 (node 3) took its own group-2 snapshot (2.4 s, 655 MB).
- 12:19:13Z: fcstor005 re-established a burst of RDMA connections, logged
  `report-split ... rc=-13` (`EFS_ERR_BUSY`) and `mkdir ... rc=-15` (`EFS_ERR_NOT_PRIMARY`).
- From then on, its group-2 `commit` and `applied` stayed at **5,080,211**, while the
  leader moved past 14.5M. So the leader sent it no further entries.
- Every group-2 apply wait sent to fcstor005 hit the 400 ms ceiling:
  4,645 timeouts between 12:48 and 12:57, about 9–10M entries behind.
- 12:49:38Z: fcstor005 imported a group-2 snapshot from the leader
  (`incl=14204461`, diff of 11.4M keys in 9.8 s). It then caught up: at 12:58Z,
  `applied=17057907` equalled commit.
- Group 2 ran on 2 of 3 voters for those 30 minutes. A loss of node 2 or node 4
  in that window would have stopped group 2.

The leader logs the same pattern at 12:49:20Z, during its own group-0
and group-2 snapshots (6–8 s each, 2.2–2.5 GB): it tore down and rebuilt RDMA
connections, and creates returned `EFS_ERR_BUSY`. This matches the open
**snapshot-export stall** in [the Raft rotation report](raft-rotate-stall-20261017.md).
The new parts are that a reconnect can leave a follower without appends until
the next snapshot install, and that snapshots grow (655 MB → 2.5 GB in 37 minutes).
The cause of the replication stop is **not diagnosed**: there was no leader-side
per-peer replication counter to read, and there was no root access for tracing.

## Server hot paths

On-CPU, 30 s at 499 Hz (≈15k samples per busy core):

| Node | Role | Busy cores (of 24) | Largest inclusive paths |
|---|---|---|---|
| fcstor004 | leader g0+g2 | **~5.1** | `server_handle_conn` 21%, `efs_hash` (BLAKE3) 19%, packer `pk_job_run`/`efs_pack_validate` 14%, `.efs` catalog build (`host_query_thread`) 9%, `lsm_get` 5%, KV compaction 5%, `host_apply` 4% |
| fcstor005 | follower g0+g2, group-2 packer | ~1.5 | KV compaction (`compactor_main`/`kv_compact_locked`) 22%, fragment writes 15%, `efs_raft_recv_batch` 11%, bloom building 9%, `host_apply` 7%, `xfs_file_fsync` 4% |
| fcstor003 | follower g0 | ~1.1 | fragment writes 22%, compaction 14%, `efs_raft_recv_batch` 13%, `host_apply` 11% |
| fcstor006 | follower g2 | ~0.9 | fragment writes 27%, `efs_raft_recv_batch` 14%, `host_apply` 11%, compaction 8% |

The profile shows no CPU limit. The leader's busiest thread was at 56%, often in D state. In the
leader's thread samples, blocked threads wait in `xlog_wait_on_iclog` (XFS log
force, i.e. fsync), `folio_wait_bit` (page writeback) and futexes.

The limit is commit-to-apply latency on the single leader. Between 12:48 and
12:57, 110k group-0 and 239k group-2 requests on fcstor004 waited more than 20 ms
for their index to apply. p50/p90/p99 were 28/46/111 ms (group 0) and 34/62/175 ms
(group 2), with the applied index 24–31 entries behind (p50).

Also growing during the run: KV L0 table count on every node (leader 121 →
284, others 47–116 → 143–253 in 45 minutes). Compaction does not keep up with
this create rate. The group-2 packer (on fcstor005) reported `lag_s` rising to
300 s and 35.9 GiB unpacked.

## What this suggests

1. Spread leadership. Correction: `EFS_METADATA_BALANCE=1` does **not** do that.
   Its `host_balanced_voter()` deliberately colocates both leaders on a voter
   common to both groups. `de307a783` adds an opt-in mode 2 that spreads them
   (see the rerun below). Here node 2 carried both groups' fsyncs, hashing,
   packing validation and the catalog build.
2. Make a disconnected follower resume appends without waiting for the next
   snapshot, and add a leader-side per-peer match/next-index counter to `raft-status`.
3. The snapshot-export stall (6–8 s, and growing) remains the next gate from the rotation work.
4. With two groups and parity placement, a workload of N top-level trees gets an
   uneven split whenever N is odd. W66 (more groups, routing) is the real fix.

## Rerun with spread leaders (`EFS_METADATA_BALANCE=2`) — inconclusive

Build `e8b3f1bb1`: devel `e7f640f3d` (W143, W144) plus `de307a783`/`e8b3f1bb1`,
the new spread mode. `make_test_summary.sh` on node9901: 816 commands. The
spread-voter test expectation was wrong once and is fixed; the remaining
failure was the known W141 `test_reclaim` flake, which passed 3/3 reruns.
`orcd-test.py start --balance 2` (in `~/git/cluster`) placed group 0 on node 1
and group 2 on node 3, as intended. The first run's trees were stopped at about
1.0–1.3M files per client and kept. New per-client parent directories were
chosen with the same group per client as before.

| 60 s window | Leaders | Group-0 clients | Group-2 clients | g0 / g2 entries/s |
|---|---|---|---|---|
| Run 1, first 10 min (`08cc16af4`) | both node 2 | 593–623 files/s | 369–377 files/s | — |
| Spread, ~3 min | node 1 / node 3 | 130–138 | 136–153 | 1,637 / 2,434 |
| Spread, ~10 min | node 1 / node 3 | 93–101 | 94–96 | 1,218 / 1,564 |
| Colocated control (`--balance 1`), ~3 min | node 3 / node 3 | 124–138 | 121–134 | 1,649 / 2,121 |
| Colocated control, ~10 min | node 2 / node 2 | 356–557 | **14–87** | 5,284 / 817 |

- With spread leaders the per-client gap disappeared: both groups' clients ran at the same rate.
- **Total throughput fell about 5× in both arms, though, and it was not caused by leader placement.**
  The cluster restarted onto the first run's backlog:
  - group-2 packer about an hour behind (`lag_s≈3600`) with ~75 GiB unpacked, against 16.7 GiB for group 0;
  - group-2 GC pending ≈3.2M and 2.1M, against 0 for group 0;
  - leader L0 at ~250 tables before the run started;
  - group-0 snapshots of 3.5 GB taking 7–12 s.
- In the colocated control, group 0 recovered to run-1 speed while group 2 nearly stopped.
- The hot thread on the new group-0 leader was the packer (`pk_abandon` →
  ledger decode, BLAKE3, digest sort, KV scans: one core at 81%).
- A clean comparison needs equal starting state: a freshly formatted cluster per
  arm, or a drained backlog. Raw profile:
  `~/orcd/scratch/efs/profile-tiny-20261022/run2-spread/fcstor003/`.

The 30-minute follower stall above is a second occurrence of the W145 class
(a replica stops receiving appends after a short local stall). Here it
recovered only through a leader snapshot install.

## Reproduction

Start the workload above on each client (one screen each). During it, on each storage node:

```sh
P=$(cat /home/erbmi1/efs-runs/orcd-test/$(hostname -s)/efsd.pid)
perf record -F 499 -g -p $P -o oncpu-30s.perf.data -- sleep 30
perf report -i oncpu-30s.perf.data --no-children --sort sym --stdio
```

Group of a client tree: inode parity of `stat -c %i /tmp/efs-mount/tiny-<host>` (odd = group 0).
Leaders and per-group commit rate: `./efs-mgmt raft-status 172.16.223.5{7,8,9}:17432` and `172.16.223.60:17432`, sampled 60 s apart.
Apply waits: `raft-host: apply-sleep` and `read-sleep` lines in each `efsd.log`.

Raw data is outside Git, under `~/orcd/scratch/efs/profile-tiny-20261022/<host>/`
(`oncpu-30s.perf.data`, `top-self.txt`, `threads.txt`, `flat-self.txt`,
`children-selected.txt`). `oncpu-30s.perf.data` SHA-256 prefixes: fcstor003
`dd1f1b95b753d312`, fcstor004 `b75e9ba3ee790332`, fcstor005 `72ed4343e7ba664a`,
fcstor006 `6ecc8e4e4e70762e`. Daemon logs are in
`/home/erbmi1/orcd/scratch/efs/orcd-test/<host>/efsd.log`; client progress is in
`/home/erbmi1/orcd/scratch/efs/orcd-test/<client>/tiny-10m-08cc16af4/tiny.log`.

## Limits

- One 30 s on-CPU profile per node, taken about 39 minutes into the run. No
  off-CPU stacks: tracefs and BPF are not available without root.
- Rates are from the clients' cumulative progress counters, at 60 s and 300 s resolution.
- The follower stall was observed after the fact from logs; no A/B, no fix.
