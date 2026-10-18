# orcd-mini profile on `5d673efa8` under the tiny-file workload — 2026-10-18

**Profiling only (no code change). No new hot path. The two waits removed
by `h->mu` steps 2–3 stay gone under load, and the leader's lock waits fell
from 15.6% to 11.6% of its context switches. The remaining time is in
proposals on `h->mu` (44% of lock waits), the pump (11%) and REPORT's
durable marking (7%).** One side effect: the test filled orcd-mini's quota;
see [Incident](#incident-quota-filled).

Follows the [`9099380f3` profile](orcd-mini-profile-9099380f3-20261017.md)
(the plan's live baseline) and the
[`5d673efa8` deploy](orcd-mini-deploy-5d673efa8-20261018.md).

## Environment and workload

- **Build:** orcd-mini on `5d673efa8fca`, daemons and both clients. Three
  efsd on node9901, RDMA `mlx5_0`, direct I/O, files engine, `.efs` on,
  `EFS_WRITE_PROF` off.
- **Leadership:** node 3 led both groups in every round.
- **Workload:** started 2026-10-18T04:40:53Z on node9900 with the user's
  recipe (`launch.sh`, as in `scripts/orcd_mini_workload_profile.sh`,
  with a 900 s cap):
  - `tiny_files.py --workers 16` into a new `tiny-files-prof-hot` tree;
  - `tiny_files_md5.py --workers 16` over `tiny-files6`;
  - `ecrawl --no-write` over the mount.
- **No other users** on either mount (`fuser -m` empty).

| Measure | `5d673efa8fca` (this run) | `9099380f3` (2026-10-17) |
| --- | --- | --- |
| Writer | 1,649 files/s sustained over 652 s | 933 + 745 writes/s from two writers |
| md5 reader | 1,336 hashes/s, 985,698 files over 738 s | 609 reads/s |
| Raft commits/s (10 s) | group 0 8,641 / 9,209 / 10,431; group 2 50–186 | 9,783 total |
| efsd CPU, three daemons | 9.0 / 9.0 / 9.6 cores per round | 12.8 cores |
| Server context switches, 10 s | 318,222 / 350,531 / 357,460 | 415,241 |
| Mutex waits, share of all efsd switches | 7.0% / 7.6% / 7.7% | — |
| Leader lock waits, share of its switches (DWARF, 3 s) | 11.6% / 11.5% / 11.6% | 15.6% |
| Condvar re-acquire, share of leader lock waits | 8.2% / 6.9% / 7.5% | not in the top 12 |

The workloads differ: the 9099 sample had two writers, a delete and two
crawlers. Rates are therefore not a regression comparison. The lock-wait
mix is comparable.

## Leader lock waits (`h->mu` and others)

DWARF-unwound context switches of the leader efsd (3 s per round), as a
share of its lock waits:

| Site | r1 | r2 | r3 | `9099380f3` |
| --- | --- | --- | --- | --- |
| `host_propose_impl` (submit + `host_propose`) | 43.3% | 44.2% | 44.5% | 27.9% (submit) |
| `host_pump` | 11.3% | 11.8% | 11.3% | 2.2% |
| `host_report_release` (REPORT) | 6.7% | 7.0% | 7.1% | 13.9% (all of REPORT) |
| `host_read_index_admitted` (LOOKUP, lanes) | 6.9% | 8.0% | 6.8% | 3.2% + 2.3% |
| Condvar re-acquire in `host_waiter_sleep` (ReadIndex) | 6.7% | 5.7% | 6.3% | — |
| `host_inbox_batch_handoff` | 2.6% | 2.1% | 2.9% | — |
| `efs_raft_disk_sync_depth` under propose | 3.6% | 3.6% | 3.4% | — |
| `host_leader_cover_pick` | — | — | — | 8.9% |
| `host_wait_verdict` | 0.8%* | — | — | 3.8% + 3.4% |

\* Condvar wake inside `host_wait_applied`, not the ring read.

The changes since `9099380f3` behave as intended:
- **Removed:** the retained-CREATE cover pick and the verdict-read waits.
- **Shifted:** the waits that remain move to the other `h->mu` holders,
  `host_propose_impl` and `host_pump`. That matches the private A/B on
  the [steps page](raft-host-mu-steps-20261017.md).
- **New:** no new lock site appears. `host_namespace_leader_hint` takes
  `h->mu` too, but the leader returns before locking, so it does not show
  up here.

## Leader on-CPU (10 s, 499 Hz)

By DSO: kernel 63%, efsd 17%, libc 10%, xfs 3.6%, libmlx5 3.1%, vdso 2.7%.
The largest self symbols are the same in all three rounds:
- kernel scheduler load balancing (`update_sg_lb_stats` 5.1–5.2%);
- KV memtable search (`kv_mtab_pos` 3.1–3.6%);
- `memmove` (2.4–2.6%);
- syscall entry/return and poll.

User-space call trees with the largest share (children), round 3:

| Path | Share of leader CPU |
| --- | --- |
| `server_handle_conn` (all RPC handlers) | 44.9% |
| `lsm_get` (KV reads) | 6.9–8.0% |
| `efs_meta_apply_getattr` (LOOKUP and GETATTR row reads) | 5.3–6.5% |
| `kv_mtab_pos`, reached from LOOKUP/GETATTR via `efs_meta_apply_getattr` | 4.2–4.8% (3.1–3.6% self) |
| `host_pump` | 4.3–5.1% |
| `host_propose_impl` / `host_apply` | 2.4–2.6% / 2.2–2.6% |
| `host_query_thread` → `efs_query_catalog_build_disk_checked` | 7.9% in r3 only, 0 in r1–r2 |

The catalog build is the periodic `.efs` refresh (60 s default); a 10 s
sample catches it about one time in six. While it runs it is the largest
user-space path on the leader. That is the cost the
[60 s default](orcd-mini-gc-query-catalog-20261017.md) was chosen to bound,
not a new path.

## Latency probe (200 files per mount, during each round), p50 / p90 / p99 ms

| Op | node9901 r1 / r2 / r3 | node9900 r1 / r2 / r3 |
| --- | --- | --- |
| create | 2.36/3.10/19.6; 2.50/3.78/29.6; 2.46/3.40/5.5 | 3.44/4.94/20.4; 3.09/4.91/48.2; 3.41/5.03/40.7 |
| stat | 0.98/3.86/14.5; 1.06/1.63/2.1; 0.95/1.68/19.1 | 1.79/4.47/50.1; 1.66/2.58/31.8; 1.81/2.40/17.2 |
| open | 1.70/6.57/26.4; 1.85/2.84/4.9; 1.66/3.42/26.6 | 2.47/11.7/72.9; 2.33/4.22/46.5; 2.57/4.09/17.0 |
| unlink | 1.53/2.77/11.5; 1.56/2.31/51.2; 1.42/1.95/2.6 | 2.59/5.99/35.5; 2.20/2.83/13.0; 2.44/3.40/34.5 |

## Reading: candidates, by evidence

1. **Proposal serialization on `h->mu`** (44% of leader lock waits; plan
   step 6, the bounded submission queue).
   - It is now the clear majority of lock waits, but lock waits are only
     11.6% of leader switches.
   - Step 1's phase timing (`EFS_WRITE_PROF=1`, a restart to enable) would
     show whether the time is lock hold, `persist` or `commit_apply`
     before a queue is built.
2. **`host_pump` holding `h->mu`** (11%). This grew as other holders left;
   it is part of the same ownership question as step 6.
3. **REPORT's durable marking** (`host_report_release`, 7%; plan step 5).
   This is smaller than the 13.9% REPORT share on `9099380f3`. The step
   still needs its audit before any change.
4. **Read path CPU:** LOOKUP and GETATTR spend 5–8% of leader CPU in
   `lsm_get`, and `kv_mtab_pos` alone is 3–4%. This is a CPU path, not a
   lock, and was not in the plan. It is worth a look if read-heavy
   workloads matter.
5. **Kernel scheduler and poll overhead** (63% of leader samples in the
   kernel). This comes from thread wakeups across connection threads,
   which the RPC model sets. It is not a single hot function.

## Incident: quota filled

- **What happened:** the writer had no file cap within the 900 s window,
  and orcd-mini was already at 91.3% (657 of 720 GiB per node). After
  about 1.1 M files it stopped with ENOSPC on close at 04:54Z. The quota
  was full: 719.88 of 720 GiB per node, 256 MiB free.
- **Effect:** for a few minutes the user's own writes would also have
  failed.
- **Recovery:** the test tree was deleted from node9900 straight away;
  `rm -rf tiny-files-prof-hot` finished at 05:03:50Z.
  - GC then reclaims about 0.23 GiB per node per minute (about 3,500
    fragments/min). That is its normal rate, and no setting was changed.
  - Usage per node: 719.88 GiB at 04:55Z, 717.1 GiB at 05:08Z and
    715.4 GiB (99.4%) at 05:15Z.
  - Back to the pre-test ~657 GiB takes about 4 more hours at that rate.
  - Small writes succeed meanwhile. A sustained write load can hit ENOSPC
    until enough is reclaimed.
- **Lesson:** cap the writer by file count (`--max-files`) to fit the free
  quota, or check `efs-mgmt status` headroom first.

## Reproduce

On node9901, while the workload runs, from a copy of the scripts in
`/data1/efs/prof/mu-prof/bin/`:
- `orcd_mini_lockprof.sh`, `cs_dwarf_locks.py` and `cs_lock.py` from
  `scripts/`;
- `rounds.sh`: three rounds, 90 s warm-up, 150 s between rounds, the
  node9901 probe and an efsd CPU delta per round.

Start the workload on node9900 with the `launch.sh` heredoc from
`scripts/orcd_mini_workload_profile.sh`, using `MD5_DIR=tiny-files6`, a
900 s `timeout`, and a `--max-files` that fits the free quota. Run
`/data1/efs/orcd-mini-client/prof/probe.py` on the node9900 mount at each
round's start.

Prerequisites: `perf_event_paranoid <= 1` and no other mount users.
Afterwards, remove the writer tree.

Raw output (outside Git):
- node9901 `/data1/efs/prof/mu-prof/`:
  - `r{1,2,3}.txt` (sha256 prefixes `2092cc48b5d812fd`,
    `75ae1e7bb185d6d3`, `7146a461d456c97d`);
  - `r{1,2,3}/` (DWARF, cs-all and on-CPU `perf.data`, 2.9 GB);
  - `r*-probe-n01.txt`.
- node9900 `/data1/efs/orcd-mini-client/prof/wl-hot-5d67/`:
  - `writer.log` `02317b0b0fa5212f`;
  - `md5.log` `73d754abc1e40b4d`;
  - `launch.sh` `472a9064f2a24ca0`;
  - `r*-probe-n00.txt`.

## Limitations

- The workload is uncontrolled across days, and the 9099 sample was a
  different mix. Only the lock-wait mix and CPU paths are compared.
- The samples are three 3 s DWARF and 10 s on-CPU snapshots on one leader.
  Follower nodes and clients were not profiled this time.
- No per-phase timing: `EFS_WRITE_PROF` stays off on orcd-mini.
