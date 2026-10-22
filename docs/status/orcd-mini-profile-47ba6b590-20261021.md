# orcd-mini profile under the 32M-file create run, `47ba6b590` — 2026-10-21

**Profiling only (no code change); verdict: no PASS/FAIL gate.** As the
user's 32M-file `tiny_files.py` run grew from ~1.0 to ~2.6 M files, its
create rate fell by more than half: from 2,564 to 1,130 files/s. Two things
on the group leader n1 grew with it:
- LSM read-lock waits: LOOKUP → GETATTR reads queue behind apply's LSM
  write lock;
- the `.efs` query-catalog rebuild, which grew from 21 s to 58 s of every
  60 s cycle.

Directory spreading started between the first and second capture points.
The spread pass itself is cheap. The slowdown coincides with spreading but is
not shown to be caused by it.

Previous profile: [`9099380f3`](orcd-mini-profile-9099380f3-20261017.md).

## Environment and workload

- orcd-mini on devel `47ba6b590a46` (daemons and both clients; started
  21:27Z): three efsd on node9901 (ports 17432–17434), RDMA mlx5_0, direct
  I/O, quota 720 GiB per node. n1 led both groups (0 and 2, term 45) at all
  three points.
- Load on node9900 `/data1/efs/orcd-mini-mnt`: the user's run, started about
  21:47Z: `tiny_files.py --max-depth 2 --max-files-per-folder 1000000
  --max-files 32000000 --workers 16`, into `20m-tiny-files`. Each folder gets
  1–1 M files and 16 are filled at once. Folders cross the 65,536-entry
  spread threshold (`EFS_DIR_SPREAD_MIN`) after ~1 M files in total.
- node9901 `/data1/efs/mnt`: another session's core/extras/persist/ior suites
  ran 21:49–21:51:28Z and that mount was remounted about 21:51Z. Its efs-fuse
  used 0.00 cores at all three points, so the samples cover the tiny-file
  writer alone.
- Capture points (UTC): p1 21:51:28–21:52:55, p2 22:01:08–22:02:39,
  p3 22:12:39–22:14:07.

## Results

| Measure | p1 (~1.0 M files) | p2 (~1.8 M) | p3 (~2.57 M) |
| --- | --- | --- | --- |
| Creates/s (`tiny_files.py` `syscw` delta, 30 s; one write per file) | 2,564 | 1,382 (1,404 at 22:05) | 1,130 |
| Raft commits/s, group 0 / group 2 (30 s) | 0 / 10,204 | 2,235 / 3,419 | 2,153 / 2,497 |
| efsd CPU (perf samples / 499 Hz / 15 s), n1 / n2 / n3 | 4.6 / 2.0 / 2.1 cores | 6.3 / 2.9 / 2.9 | 5.9 / 2.5 / 2.4 |
| node9900 efs-fuse CPU (/proc, perf attached; upper bound) | 1.72 cores | 0.85 | 0.73 |
| Server context switches (10 s, all efsd) | 317,384 | 228,431 | 235,992 |
| Leader mutex waits (DWARF cs, 3 s) | 12.7% of switches | 9.8% | 8.2% |
| Leader LSM lock waits (`lsm_rlock`/`lsm_lock`) | 3.3% of switches | 18.4% | 15.8% |
| `.efs` catalog `build_ms` (60 s refresh) | 20.9 s | 43.7 s | 56.6 s |
| n1 `apply-sleep` (>20 ms) per ~90 s window | 265, mean 36 ms | 2,644, mean 42 ms, max 400 ms | 1,436, mean 48 ms, max 400 ms |
| n1 slow gc-passes with spread time | 0 of 80 | 54 of 69 (spread max 690 ms) | 50 of 65 (max 154 ms) |

**Correction (2026-10-21, later the same day).** This page first gave n1
30.6 and 15.9 cores at p2 and p3, from `/proc/PID/stat` deltas taken around
an attached `perf record -g`. Those figures were a measurement artifact, not
CPU bursts. A same-window A/B on n1 (`perf-accounting-ab.txt` with the raw
captures) shows the effect:
- without perf, the process total and the sum of its threads agree (4.5
  and 4.4 cores);
- with perf attached, the process total reads 6.8 cores while its threads
  sum to 4.5, and no threads start or exit.

A 5 s watcher that compared both counters for four minutes saw no thread
churn and no burst outside perf windows. The table now gives perf-sample
CPU. The capture scripts now sum per-thread counters. No work item was
opened for the supposed bursts.

### Bottleneck 1: LOOKUP reads wait on the leader's LSM lock ([W126](../backlog/work-items.md#w126))

The leader's DWARF context-switch capture shows `lsm_rlock` becoming the
largest single switch kind after spreading started (13–15% of all switches).
At p3:

| Share of LSM lock waits | Site |
| --- | --- |
| 63.3% | `lsm_get` ← `efs_txn_read_ex`; at p2, 93% of these came from LOOKUP → `efs_meta_apply_getattr` (77% through `efs_txn_reduce_read_value_ex`) |
| 13.4% | apply's write side: `lsm_batch_committed` ← `efs_kv_lsm_apply_end` ← `host_apply` |
| 7.7% | `lsm_scan_prefix` (reduce-delta prefix scans) |
| 7.1% | `kv_get_copy` |
| 3.3% | REPORT `efs_kv_lsm_l0_hot` |

Each LOOKUP's GETATTR reads an inode row and its reduce lane: a get plus a
prefix scan of the pending deltas. All KV reads share one writer-preferring
`view_rw` per node, and every exclusive acquisition drains in-flight readers
and holds back new ones. In p3, the apply thread's own blocked switches were
1,051 in `pthread_rwlock_wrlock`, waiting for readers to drain. Waiters for
applied indices then slept >20 ms 1,400–2,600 times per window
(`apply-sleep`).

Each applied batch took the exclusive side twice: once only to check two
failure flags, then for the memtable update. Each REPORT took it once more,
only to read the L0 size (`efs_kv_lsm_l0_hot`). W126 step 1 removes those
read-only exclusive phases. Steps 1 and 3 were deployed as `8fb4247ad`. Their A/B
([W126 A/B](orcd-mini-w126-ab-8fb4247ad-20261022.md)) cut leader LSM waits
2–4× but did not raise the create rate.

The `h->mu` waits did not grow (8–13% of switches). They have the same shape as on
`9099380f3`: proposals 44–49%, REPORT 9–12%, `host_pump` 7–12%.

### Bottleneck 2: `.efs` catalog rebuild cost grows with file count

`host_query_thread` → `efs_query_catalog_build_disk_checked` →
`incremental_build` accounts for 26–28% of all efsd on-CPU samples at p2/p3
(7% at p1), mostly `pread` from `seek`, plus `stat_value_same` and
`chosen_shard`. Its `build_ms` grew with file count:
- 0.1 s at 0.10 M files;
- 21 s at 1.0 M;
- 44 s at 1.8 M;
- 58 s at 2.76 M.

The rebuild now takes 97% of the 60 s refresh period. It will become
continuous within about one more million files.

### Other observations

- **Directory spreading.** No efsd line marks a split itself. Evidence:
  - spread time appears in 50–54 of n1's slow (>5 ms) gc-passes from p2 on;
  - `host_dir_migrate` is ≤0.01% of efsd on-CPU;
  - 18–140 leader DWARF stacks name migrate/spread code;
  - group 0 went from 0 to ~2.2 K commits/s after p1, consistent with spread
    entries landing in the second group.
- **Superseded-fragment deletes.** `deleter_main` →
  `server_gc_superseded_delete` → `fragment_parent_sync` (`fsync`) take 7–8%
  of efsd on-CPU during a pure-create load.
- **Clients.** The node9900 efs-fuse mostly waits: 81% of switches are
  poll in `start_thread`. Its on-CPU time is RDMA polling (`recv_poller`) and
  FUSE replies. The client is not the limit. Its node9900 build loses user
  frames, so RPC waits cannot be attributed.

### Errors since the 21:27Z start

- n1 `report-split ... rc=-13 fail=wait/-13` (`EFS_ERR_BUSY`, an
  apply-wait deadline):
  - 10 at 21:51:08Z, during the other session's ior-hard;
  - 1 at 22:01:47Z, inside the p2 apply-lag window;
  - 10 in the p3 window.
- Startup only (21:27Z): `Failed to rejoin`, `segmented RDMA setup failed`,
  and `query-refresh rc=-13` once.
- `apply create rc=-4 name=raced-2` and `name=foo`, and
  `opid-replay name=foo rc=-4`: suite test names, all three nodes alike.
- No ENOSPC, assert, abort or unclean-store line. Usage grew from 53.4 to
  76.0 GiB per node.

## Reproduce

Run from the Mac while the workload runs. Use the scripts kept with the raw
captures; the wrappers `n99.sh` and `n00.sh` ssh through `orcd-login` as
`erbmi1`.
1. Copy `snap99.sh`, `cs_prof.sh`, `cs_lock.py`, `cs_dwarf_locks.py` and
   `lsm_waits.py` to node9901 `/data1/efs/prof/p32m-47ba/`. Copy `snap00.sh`
   and `cs_prof.sh` to node9900 `/data1/efs/orcd-mini-client/prof/p32m-47ba/`.
2. At each point, start `screen -dmS p32m-snapN bash -c 'bash snap99.sh pN >
   pN.txt 2>&1'` on node9901, and the same with `snap00.sh` on node9900.
   `snap99.sh` runs:
   - 30 s raft-status deltas on each node;
   - 15 s /proc CPU and `perf record -F 499 -g` of the efsd and the mount's
     efs-fuse;
   - 10 s `perf record -e cs -g`;
   - 3 s `perf record -e cs --call-graph dwarf,16384` on each group leader
     (`role=LEADER` in its own raft-status);
   - efsd log signal counts for the capture window.
   `snap00.sh` measures 30 s `/proc/PID/io` of `tiny_files.py`, plus CPU,
   on-CPU and cs of the efs-fuse.
3. On node9901, run `perf script -i pN/dwarf-n1.data -F comm,ip,sym |
   python3 lsm_waits.py`.
4. For the file-count trend, take `query-publish` lines (`files=`,
   `build_ms=`) from `/data1/efs/logs/efsd-n1.log`.

Read-only: non-root perf on existing PIDs only; nothing was restarted,
remounted or written on the mounts.

Raw captures are kept outside Git:
- Mac `~/git/devops/profiling/runs/20261021-orcd-47ba6b590-32m/`:
  `p1/node9901.txt` `4963f3f65782260c…`, `p2/node9901.txt`
  `3f4c10230289c19e…`, `p3/node9901.txt` `695b8626f59dffa6…`,
  `p3/node9900.txt` `14a010b46535980b…`, `query-publish-trend.txt`
  `b8f33beb9f5719a0…`;
- node9901 `/data1/efs/prof/p32m-47ba/p{1,2,3}` (perf data);
- node9900 `/data1/efs/orcd-mini-client/prof/p32m-47ba/p{1,2,3,3b}`.

## Limitations

- These are single 3–30 s snapshots of an uncontrolled, growing workload.
  The rate drop correlates with spreading, catalog growth and LSM lock waits
  together; this profile does not separate them. An A/B with
  `EFS_DIR_SPREAD_MIN` raised, or with the catalog refresh disabled, on a
  private cluster would.
- `cs_prof.sh` failed on the node9900 efs-fuse at p2 and p3 (`perf record`
  data size 0; threads exit during attach). A direct 10 s capture at
  22:15Z replaced it.
- No per-directory entry counts: `stat` reports size 0 for directories, and
  listing 1 M-entry directories was avoided.
- The p1 point overlapped the end of the other session's ior-hard by a few
  seconds.
