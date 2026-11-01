# orcd-mini profile under the user's workload, `f4a373b13` — 2026-10-16

**Profiling only (no code change). The ReadIndex lock fix holds under load.
The next bottleneck is waiter wake-up on the Raft host's `cv_mu`, followed
by proposals serialized on `h->mu`.**

Linked work:
- [RDMA credit fix on orcd-mini](orcd-mini-rdma-credit-20261016.md);
- [ReadIndex without the host mutex](orcd-mini-readindex-lock-20261016.md);
- [deployment record](orcd-mini-deploy-09f8af09c-20261016.md) (`f4a373b13`
  deployed 22:42Z).

## Environment and workload

orcd-mini: three efsd on node9901 (RDMA mlx5_0, direct I/O, files engine,
`.efs` on), with node 3 leading both groups at term 66. The client is
node9900 `/data1/efs/orcd-mini-mnt` running the user's workload, sampled
about 9–12 minutes in (from 22:57Z):
- `tiny_files.py --workers 16` (writer);
- `tiny_files_md5.py tiny-files1` (reader);
- `glances`.

Profiling was passive: `perf` as a non-root user on the existing PIDs. The
probe added 200 files.

## Results

| Measure | Value |
| --- | --- |
| Raft commits (`efs-mgmt raft-status`, 30 s delta) | group 0 7,814/s, group 2 2,251/s; about 10,100/s total (about 6,500/s on `09f8af09c`, different phase of the workload) |
| Workload (`/proc/PID/io`, 20 s) | writer 1,149 write calls/s; reader 1,059 read calls/s |
| efsd CPU, three daemons, 15 s | 16.6 cores (leader about 5.8) |
| efs-fuse CPU, node9900 | 1.9 cores |
| Loaded probe on node9900, p50 / p99 ms | create 5.36 / 39.1, stat 2.38 / 13.1, open 3.93 / 30.8, unlink 3.40 / 7.5 |
| Server context switches, 10 s | 498,419 |
| LOOKUP ReadIndex lock waits | 0.8% of switches (9.1% on `09f8af09c`) |
| Client context switches, 10 s | 352,845; 55% waiting for RPC replies (`rpc_send_recv_shard`), 9.4% for session command replies |

The client is latency-bound on server replies, not on its own CPU.

## Where the leader waits on locks

A 3 s DWARF-unwound capture of the leader (`perf record -e cs --call-graph
dwarf`) recorded 30,079 lock-wait switches:

| Share | Site |
| --- | --- |
| 45.1% | Re-acquiring `cv_mu` on wake-up inside `pthread_cond_timedwait` (`host_waiter_sleep`): 20.7% inlined callers, 19.9% LOOKUP, 4.5% `host_wait_applied` |
| 17.4% | `h->mu` in `host_propose_impl` via `server_raft_host_submit` |
| 6.4% | `h->mu` in `server_raft_host_report` (client REPORT) |
| 4.5% | `h->mu` in LOOKUP |
| 4.0% | `h->mu` in `host_leader_cover_pick` (retained CREATE) |
| 3.1% | GETATTR `host_read_inode_lanes_reuse` |
| 2.3% / 1.8% / 1.6% | publication batch / `host_create` proposals / `host_wait_verdict` |
| 1.5% | `efs_raft_disk_sync_depth` under propose |

`host_waiters_wake` (`src/server/raft_host.c`, around line 3561) holds
`cv_mu` for its whole scan of up to 512 waiter slots. It calls
`pthread_cond_signal` on each ready waiter while still holding the lock.
glibc does not hand the lock to the woken thread, so each one blocks on
`cv_mu` until the scan ends. All of them then re-acquire it in turn to
deregister.

Likely remedies, to be measured before any acceptance:
- mark waiters `notified` under `cv_mu`, then signal after unlocking (safe
  because waiters register and re-check their predicate under `cv_mu`);
- reduce per-waiter work under the shared mutex, or shard it.

Proposal serialization on `h->mu` is next after that.

## Reproduce

From the Mac, while the workload runs, using the scripts kept with the raw
captures:
1. Run `orcd_prof.sh OUT` (commit rates and on-CPU for daemons and client).
2. Run `cs_prof.sh PIDS DIR` on each node (context-switch stacks).
3. Run the 200-file probe on `/data1/efs/orcd-mini-mnt`.
4. For the DWARF capture:
   `perf record -e cs --call-graph dwarf,16384 -p <leader efsd> -- sleep 3`.

Raw captures are kept outside Git: Mac
`~/git/devops/profiling/runs/20261016-orcd-tiny-f4a373b13/` (with the
scripts) and node9901 `/data1/efs/prof/cs-f4a3*`.

## Limitations

- The workload is not controlled, so its phase differs between runs.
  Absolute rates are not a performance comparison.
- The 10 s and 3 s samples are single snapshots.
