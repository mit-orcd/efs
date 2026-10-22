# orcd-test: devel 08cc16af4 suites and IOR over TCP and RDMA — October 22

**Purpose:** user-requested run of every mounted suite on devel after the
dead-code cleanup / W127–W131 merge, once over TCP and once over RDMA, both
with `.efs` on, plus IOR at little and wide scale; fix bugs found on the way.
**Verdict: PASS** for the EFS suites and IOR below. One open platform failure
reproduced (W121, kernel; not an EFS defect). Four harness bugs fixed. Not a
release gate.

| | |
|---|---|
| Source | `08cc16af4` (devel), `EFS_GIT_ID=08cc16af4f77`, built on every host by `~/git/cluster/orcd-test.py deploy` (efsd built 2026-10-22T04:32:30Z); SHA-256 prefixes `efsd` 21addb7467c6a213, `efs-mgmt` 8b2ae424a3d8a97c (fcstor003), `efs-fuse` 973d4e325ce98e35 (fcstor007) |
| Harness fixes | branch `claude/orcd-test-suites`: `59d60e30d`, `e4bd1b6d7`, `ce3c20591` (tests and scripts only; daemons unchanged), copied over the deployed `tests/` |
| Cluster | fcstor003–006 storage (4 voters), fcstor007–013 clients; direct I/O; RHEL 9.8, kernel 5.14.0-687.39.1.el9_8, Python 3.9.25, libfuse 3.16.2 |
| Features | `EFS_COMMANDS_SERVER=1`, `EFS_COMMANDS=1` on every mount, query refresh 60 s (default), expiry off |
| Transport | round 1 TCP; then full stop, `start --transport rdma`, round 2 RDMA (`mlx5_2`, segmented-v1) |
| Data | stores as formatted earlier on October 22 ([previous run](orcd-test-cleanup-w127-20261022.md)); not reformatted |

## Results

| Suite | TCP | RDMA |
|---|---|---|
| POSIX `--suite all --jobs 16`, 7 clients at once | 218 PASS, 0 FAIL, 3 SKIP on every client | the same on 6 clients; fcstor010 had 1 FAIL (`virt_stats_readable`, harness bug 3 below) |
| SKIPs | `mmap_write_read` (ENODEV by design, W104); `virt_stats_readable`, `virt_find_query` (not indexed within the 10 s case budget) | same |
| `virt_` cases with `--timeout-s 150` | 3/3 PASS (fcstor008) | 3/3 PASS (fcstor010, after fix) |
| `posix_persist` | prepare 27/27 ×7 on TCP | verify 27/27 ×7 after a **full cluster stop, transport switch and restart** |
| `posix_2client --local` (two mounts, fcstor007) | 67/67 | 67/67 |
| `efs_commands`, `efs_command_options` | PASS | PASS |
| `efs_command_pages` | FAIL, then PASS after harness bug 1 fix | PASS |
| `virtual_interfaces --freshness-s 150` | 20/20 gate rows PASS | 20/20 PASS |
| `query_page_close`, `query_large_delta` | PASS | PASS |
| `efs_commands_load` | PASS: 179,898 files, all payloads verified, converged 59 s, create p50/p95/p99 5.0/8.2/31.6 ms | PASS: 158,474 files, converged 59 s, 4.3/25.0/37.7 ms |
| `chmod_truncate`, `concurrent_extend_eof`, `fsync_admission`, `mtime_barrier` | PASS | PASS |
| `exclusive_create_visibility --spawn` | 1000/1000 twice | 160/160 |
| same, stock fork writers (W121, informational) | 150/160; 1000-case runs 971–994/1000 on all 7 clients | 156/160 |

### IOR (`scripts/orcd_test_ior.sh`)

Every case writes with fsync (`-e`), reads back with data verification
(`-R`) and deletes its files. Wide cases reorder tasks (`-C`) so no rank
reads data it wrote. One iteration each. MiB/s:

| Case | Shape | TCP write | TCP read | RDMA write | RDMA read |
|---|---|---|---|---|---|
| little-easy | 1 rank, file per process, 1 MiB, 8 GiB | 1,688 | 1,762 | 1,747 | 1,842 |
| little-hard | 1 rank, shared file, 47,008 B, 0.90 GiB | 304 | 193 | 359 | 204 |
| wide-easy | 7×8 ranks, file per process, 1 MiB, 112 GiB | 8,258 | 23,043 | 5,862 (repeats 5,902, 4,214) | 32,693 (31,958, 25,614) |
| wide-hard | 7×8 ranks, shared file, 47,008 B, 4.66 GiB | 114 | 677 | 117 | 945 |

All cases rc 0, no verification errors. Over RDMA, wide-easy **write** was
below the single TCP sample in all three runs (4.2–5.9 vs 8.3 GiB/s), while
its read was above. There was one TCP sample and no profile, so this is
recorded as a measurement, not diagnosed. Wide-hard write (~115 MiB/s for
56 ranks) is the slowest case on both transports.

## Findings

**W121 reproduced (OPEN, kernel; [diagnosis](orcd-w121-fork-read-20261016.md)).**
`exclusive_create_visibility` forks its two writers from 16 worker threads.
On every client, 0.6–2.9% of first reads returned one wrong byte (0x00,
0x01, 0x02 or pointer-like 0x10/0xb0/0xe0…) while the stored file was `x`.
A temporary tracing `efs-fuse` (only stderr lines added in `read.c` and
`ll_read`; SHA-256 15911bf2c5975ec8…, mounted at `/tmp/efs-mount3`, then
unmounted) showed `efs_client_read` returned `x` and `fuse_reply_buf` sent
`n=1 b0=78` to the reading thread for every failed case. The same workload with
`posix_spawn` writers failed 0 of 2,000; with one worker it failed 0 of 1,000.
This matches W121 (FUSE direct I/O pinned with FOLL_GET, fixed upstream by
`738adade96b2`). The EFS O_EXCL/EOF check now has a `--spawn` mode; the stock
form stays in the driver as the informational `w121_exclusive_fork` row.
W121 stays OPEN. This run adds no kernel A/B.

**Harness bugs fixed (tests only):**

1. `efs_command_pages` gave a new directory 30 s, but a directory stays
   "not indexed yet" (exit 75) until the next 60 s catalog refresh; it now waits
   up to 150 s for exit 75 only (`59d60e30d`).
2. Reported from orcd-mini by another session on this build:
   `tests/faults/harness.py` still passed the removed export label to efs-fuse
   (regression from `74c283293`), so every `--integrate` case failed at
   client start; `query_timing.refresh_seconds` failed with exit 75 on a
   freshly formatted cluster. Both fixed in `e4bd1b6d7`. Neither path ran on
   orcd-test; the orcd-mini rerun belongs to that session.
3. `posix_suite.efs_command_output` could start an `.efs` retry with ~0.01 s
   of case budget left, turning the expected budget SKIP into a
   `TimeoutExpired` FAIL (fcstor010, RDMA). Retries now need 1 s left (`ce3c20591`).
4. The new IOR driver sources `mpivars.sh` under `set +u`.

## Reproduction

From the Mac with `~/git/cluster` (cluster already formatted and exported):

```sh
python3 orcd-test.py deploy --source ~/git/efs --ref devel
python3 orcd-test.py start --transport tcp --commands     # round 2: --transport rdma
python3 orcd-test.py mount --transport tcp --commands
```

Copy the fixed `tests/` from `claude/orcd-test-suites` over `/tmp/efs/tests`
on the clients, or deploy that ref. Then, from `/tmp/efs`:

- Each client: `python3 -u tests/posix/posix_suite.py /tmp/efs-mount/posix-<transport>-$(hostname -s) --suite all --jobs 16 --results posix-all.tsv`,
  and `--suite extras --filter virt_ --timeout-s 150` on one client.
- Persist: `tests/posix/posix_persist.py /tmp/efs-mount --phase prepare` on all clients, then
  `orcd-test.py stop` + `start`/`mount` (other transport), then `--phase verify`.
- fcstor007: `EFS_TRANSPORT=<t> EFS_RDMA_DEV=mlx5_2 EFS_COMMANDS=1 bash scripts/client.sh 172.16.223.57:17432 /tmp/efs-mount2`,
  then `bash scripts/orcd_test_live_suites.sh OUTDIR` (writes `summary.tsv`;
  `w121_` rows do not count toward its PASS line). About 30 minutes.
- fcstor007, with an ssh agent that reaches every client (mpirun):
  `bash scripts/orcd_test_ior.sh OUTDIR /tmp/efs-mount`. Run it alone, so it doesn't overlap other load.
- Cleanup: `bash scripts/client.sh stop /tmp/efs-mount2` before `orcd-test.py stop`.

Raw output stays on the cluster under `/home/erbmi1/orcd/scratch/efs/orcd-test/`:
`<client>/suites-{tcp,rdma}-08cc16af4/` (POSIX TSVs, persist, `live/` logs and
`summary.tsv`), `fcstor007/ior-{tcp,rdma}-08cc16af4/` (IOR logs, `summary.tsv`,
RDMA repeats), and `fcstor007/w121-recheck-20261022/` (trace log, diagnostic
sources, JSON). The W121 fixtures that failed are kept in `/tmp/efs-mount` as
`exclusive-visibility-*` (12 directories).

## Limits

- One run per suite per transport; IOR one iteration per case (RDMA wide-easy three).
- The persist verify crossed a restart and a TCP→RDMA switch, but not a
  remount on the same transport within the RDMA round.
- No fault injection, no node loss. The fault suite and the
  orcd-mini gates fixed in `e4bd1b6d7` were not rerun here.
- W121 needs a kernel with `738adade96b2` or an equivalent backport. That remains deferred.
