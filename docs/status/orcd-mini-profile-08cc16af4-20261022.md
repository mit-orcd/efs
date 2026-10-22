# orcd-mini profile on `08cc16af4` under two tiny-file writers — 2026-10-22

**Profiling only; verdict: no PASS/FAIL gate.**
- **Throughput:** two `tiny_files.py` writers created about 3,000 files/s
  together. The limit is the leader n1, which leads both groups:
  - `h->mu` proposal waits;
  - REPORT;
  - apply lag (`apply-sleep` 1,290 per ~90 s window, mean 28 ms).

  These are the known [`h->mu` plan](raft-host-mu-plan-20261016.md) items.
- **W126 still holds:** leader LSM lock waits are 1.4% of its switches.
- **New item:** [W143](../backlog/work-items.md#w143). efs-fuse spends ~4%
  of its CPU in `getenv`, reading diagnostic switches on per-op paths.
- **Not new:** each client used only 0.16–0.18 cores, so neither client is
  the limit.

Previous profile: [`47ba6b590`](orcd-mini-profile-47ba6b590-20261021.md).

## Environment and workload

orcd-mini on devel `08cc16af4f77` (daemons and both clients). The cluster
appears freshly formatted (Raft term 4, 0.35 M files at capture time).
Three efsd on node9901 (ports 17432–17434), RDMA mlx5_0, direct I/O, quota
720 GiB per node. n1 led both groups.

| Client | Workload (started ~6 min before capture) | Creates/s (`syscw`, 30 s) | efs-fuse CPU |
| --- | --- | --- | --- |
| node9900 `/data1/efs/orcd-mini-mnt` | `tiny_files.py --max-depth 2 --max-files-per-folder 1000000 --max-files 32000000 --workers 16 …/20m-tiny-files` | 1,413 | 0.16 cores |
| node9901 `/data1/efs/mnt` | `tiny_files.py --max-depth 8 --max-files-per-folder 10000 --max-files 20000000 --workers 16 …/tiny-files1` | 1,561 | 0.18 cores |

Capture window: 2026-10-22 05:26:44–05:28:22Z, passive (non-root perf on
existing PIDs). A second node9901 mount for another session's suite
(`/data1/efs/mnt-vi-peer`) had ended before the capture.

## Results

| Measure | Value |
| --- | --- |
| Raft commits/s, group 0 / group 2 | 6,232 / 6,936 (13,168 total) |
| efsd CPU (thread sums, 15 s), n1 / n2 / n3 | 4.63 / 2.07 / 2.10 cores |
| Server context switches (10 s, all efsd) | 376,434; mutex waits 10.2% |
| Leader lock waits (DWARF cs, 3 s) | 15.4% of switches: proposals 50%, REPORT `host_report_release` 15%, `host_pump` 5% |
| Leader LSM lock waits | 1.4% of switches (`kv_get_copy` gets 56%, apply commit 16%) |
| n1 `apply-sleep` | 1,290 in the window, mean 28 ms, max 117 ms |
| efsd on-CPU, largest user subtrees | connection handling 26%, `.efs` catalog build 18% (`build_ms` 7.9 s at 0.35 M files), fragment writes 12%, `efs_meta_apply_getattr` 12%, Raft pump 9% |
| Client switch kinds (DWARF) | node9901: 61% RDMA receive poller, 28% `meta_flush_main` sleeps; node9900: 79% / 15% |
| Errors | none; 7 `report-split rc=0` lines, `delete_errors=0`, cluster OK, 3/3 identical versions |

### Candidates checked

- **`getenv` in efs-fuse: 2.9% self, 3.7% with children, plus 1.8%
  `strncmp` and 1.1% `strlen`.** A DWARF on-CPU capture attributes it to
  `dir_lock_take` ← `efs_client_lock_all_dirs` ← `evict_one`. The source
  shows more per-op reads (lookup memo, flush report, opid trace). Filed as
  **W143** and fixed.
- **Client `write` syscalls (~38 per created file).** These are
  `pend_push` ← `harvest_recv_wcs` eventfd wake-ups. W47 already limits
  them to waiters that have armed the eventfd, so this is not waste.
- **Server `make_dir_for_file` → `mkdir` (4.8% of efsd).** The walk is
  already leaf-first (Sep 29), and `mkdir` self time is 0.02%. The cost is
  XFS creating new fragment directories. That is a layout question, not a
  small fix.
- **`.efs` catalog incremental build (18% of efsd).** Known (W106 and the
  2026-10-21 profile).

## Repeat on `46496416e` (W143 deployed), p2 — 2026-10-22 12:28Z

- **Deploy.** Release `46496416e9c5` was built on both nodes:
  - efsd `c6254fcf9a5262e7…`;
  - node9901 efs-fuse `85234e8336fc6f9b…`;
  - node9900 efs-fuse `9e80ee300137ebde…`.

  The user's 32M writer (PID 1214638) was stopped with SIGKILL. Clients
  were stopped with plain `client.sh stop`, then `stop.sh`; all three roots
  logged `stopped clean`. The symlinks were switched and `start.sh` started
  the cluster on preserved storage: 3/3 identical versions, POSIX smoke
  216/216.
- **Workload.** The same two `tiny_files.py` commands, into fresh
  directories (`w143-tiny-files1`, `w143-20m-tiny-files`). Captured about
  6 minutes in, then both writers were stopped.
- **W143 verified.** `getenv`, `__strncmp_evex` and `__strlen_evex` no
  longer appear in either client's on-CPU profile, nor in a DWARF on-CPU
  check of the node9901 client. In p1 they were 2.9% / 1.8% / 1.1%.
- **Throughput is not comparable to p1.** The cluster had grown from 0.35 M
  to 15.95 M files. Creates were 592 + 204/s. `apply-sleep` ran about 10.5 K
  per window with a mean of ~80 ms, against 1,290 with a mean of 28 ms in
  p1.
- **Snapshot export.** 32% of efsd on-CPU was `host_gc_thread` →
  `host_snap_export_pass` → `efs_kv_lsm_view_export` (`vx_sift_down`,
  `memcmp`). A group-0 export (5.3 GB, 16 s) ran from 12:28:51 to 12:29:07
  and overlapped the capture. Exports are about 50 minutes apart here. The
  cost is the known one recorded in the work-item history; no new item.
- **New: [W144](../backlog/work-items.md#w144).** Leadership was split (n2
  led group 0, n1 led group 2). On n1, `host_namespace_leader_hint` ←
  `server_raft_host_lookup` was 22.1% of lock waits: every LOOKUP/GETATTR
  at a group follower took `h->mu` to read the voters. Fixed with the
  lock-free published view.
- Raw: Mac `p2/node9901.txt`, `p2/node9900.txt` and `p2/getenv-check.txt`;
  node paths as for p1, under `p2`.

## Reproduce

Scripts are kept with the raw captures in
`~/git/devops/profiling/runs/20261022-orcd-08cc16af4/`:
- `profile_all.sh POINT` runs `snap99.sh` on node9901 and `snap00.sh` on
  node9900 in screens `prof-POINT`. Each captures:
  - 30 s Raft commit deltas and per-process I/O syscall rates (`topio.sh`);
  - 15 s per-thread CPU and `perf record -F 499 -g` of every efsd and
    efs-fuse;
  - 10 s `perf record -e cs -g`;
  - 3 s `perf record -e cs --call-graph dwarf,16384` of each group leader and
    each efs-fuse;
  - daemon log signals.
- The attribution of the `getenv` callers comes from `perf record -F 299
  --call-graph dwarf,16384` (8 s) on the node9901 efs-fuse.

Raw captures stay outside Git:
- Mac `p1/node9901.txt` `f1b5eb7e28cae802…` and `p1/node9900.txt`
  `c8bc027b00376909…`;
- node9901 `/data1/efs/prof/orcd-08cc16af4/p1`;
- node9900 `/data1/efs/orcd-mini-client/prof/orcd-08cc16af4/p1`.

## Limitations

One 3–30 s snapshot early in both runs (0.35 M files), with uncontrolled
workloads. Throughput limits at large directory sizes are in the
[2026-10-21 profile](orcd-mini-profile-47ba6b590-20261021.md) and the
[W126 A/B](orcd-mini-w126-ab-8fb4247ad-20261022.md).
