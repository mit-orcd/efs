# orcd-mini fresh-format deploy of devel with D25, suites and step-5 gates — 2026-10-22

**Deployed; all suites and D25 step-5 gates PASS after two `.efs` startup
fixes (found and fixed during this run).** User request: deploy latest
devel to orcd-mini and the node9900 client, run all test suites and
[D25 step 5 gates](d25-step5-gates-20261022.md), and fix bugs found on the
way, all on devel. D25 needs a fresh format, so the user authorized a full
wipe of orcd-mini's stores. No backward compatibility (AGENTS.md).

## Deploys

| Time (UTC) | Build | Change |
| --- | --- | --- |
| 02:54 | `49679a57072c` | Clean stop of both clients and all daemons (all roots `stopped clean`). `/data1/efs/n1`–`n3` and `.mkfs-done` were wiped and the daemons started fresh (`raft-mkfs rc=0`), with both mounts remounted. All previous data was erased, including `20m-tiny-files`, the W126 A/B directories and `c6`. |
| 03:21 | `cca8868df312` | Adds `ef7b1735d` (first `.efs` fix). Same format; storage kept. |
| 03:36 | `ee1087129161` | Adds `ff93f8e86` (second `.efs` fix). Same format; storage kept. |

Builds come from a clean `git archive` with a shared build ID, the private
libfuse 3.16 and `EFS_START_TIMEOUT=900`. Before every restart, `fuser -m`
was empty on both mounts. `efs-query` was removed from devel in `e1cbfa395`,
so release builds drop it. devops `deploy.sh` still built it, fixed in devops
`56725a4`.

Final binaries:
- node9901 `/data1/efs/releases/ee1087129161`: efsd
  `e02b01d9966f05936a74e82676a01f3664445da01fdd97ee6e715bab3bd1a290`,
  efs-fuse `5ff51ca363dd21ec5e82fdecd5e40cc15a5087ba38c4d4734caa9ca5eed73a10`.
- node9900 `/data1/efs/orcd-mini-client/releases/ee1087129161`: efs-fuse
  `bf1c155cdc919249c29ffce00a0dfa2ddda469dc886f8a707d42c676923d7f33`.

## Results

Environment: orcd-mini, three efsd on node9901 (RDMA mlx5_0, direct I/O,
720 GiB quota per node), clients on node9901 `/data1/efs/mnt` and node9900
`/data1/efs/orcd-mini-mnt`. The private suites ran on node9901 under
`/data1/efs/evidence/testsuite` and `/tmp` on their own ports.

| Suite | Build | Result |
| --- | --- | --- |
| Core POSIX (`smoke.sh`, jobs 4) | `49679a570`, `cca8868df`, `ee1087129` | 216/216 PASS each time |
| POSIX extras (`posix_extras.py --timeout-s 150`) | `49679a570`, `cca8868df` | **FAIL**: `virt_stats_readable`, `virt_find_query` (EBUSY; fixed below) |
| POSIX extras | `ee1087129` | 4/5 PASS, 1 SKIP (mmap unsupported) |
| Durability (`smoke-persist.sh`) | `49679a570` | 27/27 + 27/27 PASS |
| ior easy / hard (8 ranks, OpenMPI 4.1.7) | `49679a570` | rc 0 / rc 0; easy write 1,304 MiB/s, read 6,296 MiB/s |
| Two-client (`scripts/orcd_mini_posix2.py`) | `49679a570` | 67/67 PASS (307 s) |
| `make test` (`scripts/make_test_summary.sh`) | `ef7b1735d`, then `ff93f8e86` | 812 / 811 commands, 5 environmental failures each: `test_readindex_pair`, `test_raft_ack_window`, `test_host_outbox` and `test_stage_evict_lru` fail only from the host's LeakSanitizer and pass with `ASAN_OPTIONS=detect_leaks=0`; `check-architecture` needs the absent `results/` |
| D25 step-5 gates (`d25_step2/3/4_gates.sh`, repro; `EFS_FAULTS=1` tree of `ef7b1735d`) | private node9901 cluster | **PASS**: step 2 17 checks, step 3 39, step 4 1,454, repro 2; 0 FAIL |
| GC + POSIX, RDMA direct (`gc_reclamation.py --transport rdma --posix`) | `ef7b1735d` tree | PASS (71 checks), after relinking efs-fuse (runner bug below) |
| Fault suite, integration, RDMA direct (`run_faults.sh --integrate`) | `EFS_FAULTS=1` tree | 24/24 PASS |

Not rerun on `ee1087129`: durability, ior and two-client. The two fixes
change only `.efs` query scheduling and error mapping.

## Bugs found and fixed (all on devel)

1. **`.efs` stats/find failed (EBUSY, exit 1) after a fresh format or
   restart.**
   - **First cause, fixed in `ef7b1735d`:** with no catalog published yet,
     `server_raft_host_query_read` returned BUSY. The client treats BUSY as
     a hard failure. It now returns NODATA ("not indexed yet", exit 75), as
     `9099380f3` already did for directories newer than the catalog.
   - **Root cause, fixed in `ff93f8e86`:** the leader's first capture after
     every restart is refused BUSY while authority settles. The failure
     backoff then waited a full refresh interval (60 s) before retrying.
     Now, while no catalog exists, a capture refused with BUSY or
     NOT_PRIMARY (which builds nothing) is retried after 1 s, and a
     transient refusal with no catalog answers NODATA. Failed builds keep
     their backoff.
   - **Measured:** private three-node restart A/B
     (`scripts/query_restart_ab.sh`, a new directory queried from the first
     second). Before: 7 EBUSY failures, first success at 60.8 s. After:
     0 failures, first success at 1.5 s. On orcd-mini the catalog now
     publishes 1.1 s after the refused capture.
   - Tests: `test_query_target_route` and `test_query_refresh`.
2. **`scripts/make_test_summary.sh` reported an empty run as a pass**
   (`b3fe6b9b5`). It ran from `scripts/` instead of the tree root, built
   nothing and wrote `TOTAL 0 FAIL 0`. It now requires a Makefile and fails
   on a build failure or an empty command list.
3. **`make_test_summary.sh` left an efs-fuse without the libfuse rpath**
   (`cca8868df`). `make -n test` ran without the build's `FUSE_LIBS`, so a
   relink inside the test recipe dropped the rpath. That broke the GC gate
   with "FUSE_3.12 not found".

## Reproduce

Commands are as in the table. Use
[d25-step5-gates-20261022.md](d25-step5-gates-20261022.md) for the gates
and `bash scripts/query_restart_ab.sh <release-or-tree> <label>` on node9901
for the restart A/B.

Raw outputs stay outside Git on node9901:
- `/data1/efs/prof/suites-20261022-0258/`;
- `/data1/efs/evidence/testsuite/{suite,fault}-49679/` (`summary.txt`, `d25-out/`);
- `/data1/efs/evidence/testsuite/gcfault-out/`.

## Limitations

- The orcd-mini data from before 02:54Z is gone (authorized wipe).
- LeakSanitizer remains broken on node9901.
- Valgrind gates were not run.

**Raw evidence note (2026-10-23):** the bulk data in `testsuite` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
