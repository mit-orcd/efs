# orcd-mini deploy of devel `08cc16af4` (dead-code cleanup merge), RDMA + `.efs` suites — 2026-10-22

**Deployed; all suites and gates PASS.** Two private gates first failed
from test-harness bugs; the cleanup session fixed both in `e4bd1b6d7`
(branch `claude/orcd-test-suites`, not yet on devel) and both gates pass
when rerun with those files. User request: deploy devel on
orcd-mini, run all test suites over RDMA with `.efs` commands only, and
report bugs to the session "Dead-code cleanup from the 2026-10-21 audit".

## Build and environment

- Source `08cc16af4f77` (merge of `claude/dead-code-cleanup-5f7087`: dead-code
  cleanup and W127–W131 apply hardening). Clean `git archive`, shared
  `EFS_GIT_ID`, private libfuse 3.16.
- node9901 `/data1/efs/releases/08cc16af4f77`: efsd
  `fa54ed634c0f37ae20f4447f5090c5e9e9dff83d5139f05ef49be69e5dd381d1`,
  efs-fuse `141e4ff284e293eaa29a21781eebdf1bda27ab042c41dda2b49d2da9db977d52`;
  node9900 efs-fuse
  `e0ce5261e36a2a9619de0f626c03678c25c79030560327d820bc3e9ad1e7e514`.
- orcd-mini as in [the D25 deploy](orcd-mini-d25-deploy-20261022.md): three
  efsd on node9901, RDMA mlx5_0, direct I/O, 720 GiB quota; clients on
  node9901 and node9900. Same D25 format, so no wipe; `fuser -m` was empty on
  both mounts before the stop; no incompatible log entries at start.
- Private gates ran on node9901 from an `EFS_FAULTS=1` tree of the same
  commit, transport RDMA.

## Results

| Suite | Result |
| --- | --- |
| Core POSIX (`smoke.sh`) | 216/216 PASS |
| POSIX extras (`posix_extras.py --timeout-s 150`) | 4/5 PASS, 1 SKIP (mmap) |
| Durability (`smoke-persist.sh`) | 27/27 + 27/27 PASS |
| ior easy / hard (8 ranks, OpenMPI 4.1.7) | rc 0 / rc 0; easy write 1,329 MiB/s, read 4,391 MiB/s |
| Two-client (`scripts/orcd_mini_posix2.py`) | 67/67 PASS (264 s) |
| `virtual_interfaces.py --freshness-s 150` | 11/11 PASS (`nested_authorization` needs `EFS_QUERY_TEST_HOST=10.1.99.1 EFS_QUERY_TEST_PORT=17432`) |
| `make test` (`scripts/make_test_summary.sh`) | 816 commands, 1 FAIL: `check-architecture` (environmental, no `results/`) |
| D25 step 2/3/4 gates + repro (`EFS_TRANSPORT=rdma`) | PASS: 17 / 39 / 1,454 / 2 checks, 0 FAIL |
| `scripts/w127_incompatible_entry.sh` (`EFS_TRANSPORT=rdma`) | 19/19 PASS |
| GC + POSIX (`gc_reclamation.py --transport rdma --posix`) | 71 PASS |
| GC `.efs` commands (`gc_reclamation.py --transport rdma --efs-commands-only`) | **FAIL**, harness bug 2; rerun with `e4bd1b6d7` test files: PASS |
| Fault suite (`run_faults.sh --integrate --transport rdma --rdma-device mlx5_0`) | **FAIL** 6 PASS / 5 FAIL, harness bug 1; rerun with `e4bd1b6d7` test files: 24/24 PASS |

## Bugs (fixed in `e4bd1b6d7`, harness only)

1. **Fault harness still passes the removed export label.**
   `74c283293` removed the efs-fuse export argument, but
   `tests/faults/harness.py` still passes `EXPORT = "efs-fault"`. efs-fuse
   refuses it ("unexpected argument ... after mountpoint"), so all five
   integration cases fail at startup.
2. **`query_timing.refresh_seconds` does not tolerate "not indexed yet".**
   It runs `.efs/stats --json` with `check=True` right after a fresh mount.
   With no catalog yet, stats exits 75 (NODATA, since `ef7b1735d`), and
   `efs_commands.py` aborts. It should retry 75 under a deadline, like
   `wait_indexed`.

## Reproduce

Commands are in the table; the suite drivers and gate scripts are as in
[the D25 deploy](orcd-mini-d25-deploy-20261022.md#reproduce). Raw outputs
stay outside Git on node9901:
- `/data1/efs/prof/suites-20261022-0434/`;
- `/data1/efs/evidence/testsuite/private-08cc/` (`summary.txt`, `gc-efs.out`, `faults.out`);
- `/data1/efs/evidence/testsuite/cm8-a/gc-direct-f4c3_ke9/`;
- `/tmp/efs-fault-08cc/` (`results.jsonl`, `cluster/*/fuse-a-*.log`).

## Remaining gates

- The reruns used the `08cc16af4` binaries with only
  `tests/faults/harness.py` and `tests/live/query_timing.py` taken from
  `e4bd1b6d7` (outputs in `/data1/efs/evidence/testsuite/private-e4bd/`,
  `/tmp/efs-fault-e4bd/`). `e4bd1b6d7` is merged into devel in `99c7a1c75`.
- LeakSanitizer remains broken on node9901; Valgrind not run.

**Raw evidence note (2026-10-23):** the bulk data in `testsuite` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
