# NUC e59 focused deep/readdir confirmation — 2026-10-12

Exact manager-frozen `e59e815fab2916340baad585dc08712ed182b7ea` on
`qualification-v030-tcp-direct`, reviewed request-local READDIR fence reuse.
Archive SHA256 `3ae2aeb2a9befd7ce8e2639a74c1bc23594ee5dacac249267f2e8f0f9b222eae`.
All23,318 manager regular tracked-file hashes/commit verified before normal
build and before/after live tests. Build `e59e815f-group3-normal`; mapped actual
executable hashes and versions recorded. No candidate source edits.

Authorized resource-weighted NUC scope: lightweight correctness, not broad
matrix. Four direct/TCP/FILES daemons with4G quota each; seven clients.
Actual4+7 `/proc` environments and mapped images verify EFS_COMMANDS0,
EFS_COMMANDS_SERVER0, flow/earlyretire/groupdrain0, repair0.
`.efs` absent by actual listing/existence checks. No compiler/profiler/other
EFS daemon during measurement. Isolated `/data1/efs/evidence/nuc-e59-focus` roots,
ports24410–24413. Existing case assertions and30-second budgets unchanged.

**PASS**:

| Focused phase | Cases | Parallel time | Per-suite time |
|---|---:|---:|---:|
| Seven simultaneous core suites filtered dir_deep_nesting, jobs16 | 14/14 (60+100depth per client) | 2.334s | 1.6–1.8s |
| Seven simultaneous original dir_readdir_listing cases, jobs16 | 7/7 | 1.521s | 1.2–1.4s |

All cases0FAIL/0SKIP/0NOTRUN; all seven return0 in each phase. Original readdir
case creates50one-byte files per client (350total), checks exact Python listdir
set and GNUls set. Phases are consecutive, not a broad mixed background matrix.
60-depth case retains original growing full-path setup/rmtree; no rewrite or
extension. 600s outer per-suite caps, original30s per-case deadlines.

Existing basic_write_read case PASS. Small fsynced peer bytes/rename/unlink
visibility PASS. Normal client drain/daemon TERM cleanup has no errors; all
seven mounts unmounted and no remainingefsd. No backward qualification or
old-binary/data preservation, unrelated host paths/services untouched.

## Limits and evidence

Focused workload omits the prior broad background cases, `.efs` disabled.
No causal attribution from this pass to READDIR change or query disable; no
claim that heavy CREATE-stage timeout is fixed. Prior ecdc focusedpass already
showed fast deepcases, and broad priorfailures remain retained. AMD owns heavy
full lifecycle and finalnative. W97 comparison/fulloriginalW52/bothmodeW112 are
not closed. RDMA/repair deferred. No pushes/tags/promotions.

Raw complete source/build manifest, actual image/config proof, external runner,
commands/exit/timing, all21cases in14complete TSVs/logs and result/cleanup:
`results/measure/20261012-nuc-e59-focused/raw/`; archive retained. Remote
`/home/efs/nuc-e59-focus`. Manager informed of exact focusedpasses and limits.
No duplicate broad test or further live profiling launched; EFS cluster stopped.

**Raw evidence note (2026-10-23):** the bulk data in `nuc-e59-focus` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
