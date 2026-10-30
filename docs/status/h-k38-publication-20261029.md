# H K38/K40 publication group port (F01–F09, F25)

**Purpose:** port the proven Task H publication fixes F01–F09 and F25 from `agent/task-h-repair` onto devel `126ba81d4` (wave 37) as small commits, each with a named retained regression that fails on devel (or the prior source) and passes with the change. Linked rows: K38, K40, D4. Blocks alpha: no new blocker; the K38/K40 rows keep their VERIFY status (no real-cluster/RDMA rerun was made).

**Verdict:** F03, F04, F07, F08, F09 and the F02 retained-ownership rule PASS (below). F01 and F06 were already on devel. F05 is NOT-NEEDED on devel. F25 PASS on loopback unit/native controls; its end-to-end 14 s barrier symptom was not re-measured (NOTRUN). Gate: see *Gate*.

## Source and build identity

Branch `agent/h-k38-publication`, base devel `126ba81d4`; cherry-picks of the commits named per fix, conflicts resolved by hand (devel wins). Tested trees were `git archive` copies of the branch tip on intel2 (`~/efs-hk38`, `-O3 -march=native`, `EFS_BUILD_ID` unknown) and of devel with the branch's tests overlaid (`~/efs-hk38-base`).

## Fixes

| Fix | Result | Commits (branch / origin) | Regression (fails on devel → passes) |
| --- | --- | --- | --- |
| F01 receipt STALE within the completion budget; STALE kept across later BUSY | **Already on devel**: `completion_deadline`/`resolved_stale`, `efs_client_rpc_resolve_budget_ms` and the server retention (`84d6c351c`, empty cherry-pick) are present. Nothing ported. Its remaining test failure on devel was the F03 block. | `0c1970678`, `84d6c351c` skipped | `test_report_resolved_retry.py` (via F03) |
| F02 owned unchanged upload reused only with fresh ADMITTED ticket proof | Ported on top of devel's terminal-ticket tests. `e7b321502` was already present in source and test. | `b7ef01a76` (`40856b152`), `647d98cda` (`fdc0f0900`), `b5ef40197` (`e41cd19ad`), `4556abd9b` (`5e63df1a0`), `7249fc81e` (`1848fe5e0`) | `test_put_ticket_owner.py`: devel fails to compile (`efs_client_put_retry_admitted` undeclared); passes with the change. `test_stale_recovery.py`: devel cannot locate the branch's helper (`ValueError`, same NOCOMPILE class as the baseline). |
| F03 replay the exact sent snapshot after a late PARTIAL | Ported | `d3a5c7b73` (`84d3c8f3a`), `4383cf1e1` (`6c1f9dd1e`, keeps `test_k38_report_bound.py` aligned) | `test_report_resolved_retry.py` executes the production receipt block: devel SIGABRT, branch PASS |
| F04 REPORT keeps its live coordinator across BUSY; multi-chunk single-inode REPORT routes to the covered leader | Ported (client `inode_rpc.c` retry loop merged into devel's deadline-bounded loop; server `host_report_primary_pick`) | `bc60768e3` (`fcc44fe5c`), `9067b0b29` (`45102379f`) | `test_metadata_retry_budget.py`: devel SIGABRT, branch PASS. `test_host_cover_route.py`: devel has no `host_report_primary_pick` (`ValueError`), branch PASS with the eight-record regression |
| F05 rebuild identity only after a fully proven abort | **Not landed: NOT-NEEDED.** `test_pub_unit_forward_abort.py` and `test_fin_abort_handoff.py` pass on devel (baseline page). `a42933a72` is mostly R03 batching and is not ported. | — | — |
| F06 capacity pressure flushes only a frozen prior cohort | **Not landed:** `efs_client_report_pressure_ino`/`fuse_pressure_flush` and the cohort drain are already on devel (both commits cherry-pick empty); `test_pressure_cohort.py` and `test_put_drain.py` pass on devel. The baseline NOCOMPILE was a test-API mismatch, not a missing fix. | — | — |
| F07 retain immutable PUT through temporary quota refusal with no spare | Ported | `825815916` (`a10859489`); gate test `0962a98e2`, `4db197297` | `test_put_retry_budget.py`: devel `ValueError`, branch PASS. `test_put_quota_reclaim.py` (real-quota FUSE cluster, `test-long`): baseline FAIL on devel with ENOSPC; **NOTRUN here** (needs FUSE and three private daemons; not run in this session) |
| F08 clear every GETCHUNKS record before a collect retry | Ported | `5ec231ed5` (`121e5fb24`), test fixture prototype fix `955e5ecc1` | `test_getchunks_collect_retry.py`: devel SIGABRT (strict client validation), branch PASS |
| F09 mix allocated inode bits into dcache buckets and striped I/O locks | Ported | `5a820f91c` (`662789008`) | `test_dcache_lock_distribution.py`: devel SIGABRT, branch PASS |
| F25 exact PUT epoch owns fragment and ancestor namespace barriers | Ported with the barrier primitive substituted inside devel's K55 syncer: the epoch's checked files and names replace `syncfs` of the whole root; the epoch record, `durable_seq` and sticky-failure rule are devel's and untouched. Second commit drains queued namespace proofs before ancestor GC. | `b687e26bb` (`6412320f6`), `a1855bd1e` (`becc5729c`) | `test_put_epoch_files.py`: devel fails to compile (`put_epoch_files.h` absent); `test_store_durability` native controls added on the branch (checked-file/namespace barriers, GC drain) |

Dropped on purpose: `scripts/qualify_publication_repair.py` (forensic driver, not needed by a kept test), the branch's early COMMIT helper, and the slow-barrier counters other than the one-line diagnostic F25 already carries.

## D45 / alpha 1.1 inputs

F02 and F03 edit `write.c` publication code but only locally (retention flag on the existing put record, a replay helper); they do not change publication ownership. **F25 is the one to watch:** it edits the K55 syncer's barrier primitive in `store.c`. It does not add a scheduler or change `data_commit.c`, so it was kept, but if A11-B decides the PUT durability barrier should be one root-wide `syncfs` per epoch, revert `b687e26bb`/`a1855bd1e` as a pair. No fix was deferred wholesale; F01, F05 and F06 are no-ops because devel already holds them or does not need them.

## Reproduction

Prerequisites: a Linux x86_64 host with gcc, python3 and the repo's normal build deps.

```sh
git archive agent/h-k38-publication | tar -x -C ~/efs-hk38 && cd ~/efs-hk38
make -k -j8 && make -k -j8 test-bins
for t in report_resolved_retry getchunks_collect_retry dcache_lock_distribution metadata_retry_budget \
         host_cover_route put_ticket_owner k38_stale_span_replay stale_recovery put_retry_budget \
         put_epoch_files k38_report_bound; do python3 tests/test_$t.py; done
./tests/test_store_durability
```

Control: extract `git archive 126ba81d4 src include Makefile` over a copy of the branch tree and run the same list; expected failures are the ones in the table above. `test_put_quota_reclaim.py` additionally needs FUSE (`make test-long`).

## Results (intel2, Oct 29)

Branch tip: all eleven Python tests and `test_store_durability` rc 0 (one fixture warning-as-error under gcc fixed in `955e5ecc1`; `test_k38_report_bound` needed `4383cf1e1`). Devel source with the same tests: `report_resolved_retry`, `getchunks_collect_retry`, `dcache_lock_distribution`, `metadata_retry_budget` abort at runtime; `host_cover_route`, `put_ticket_owner`, `stale_recovery`, `put_retry_budget`, `put_epoch_files`, `k38_report_bound` fail to locate or compile branch-only code; `k38_stale_span_replay`, `pressure_cohort`, `put_drain` pass on devel.

## Limitations

- Several "fails on devel" results are structural (function/API absent), not behavioural; they show the fix is missing, not a measured user-visible failure. The behavioural evidence is the four runtime aborts (F03, F04, F08, F09).
- F02's reuse rule has no devel-side runtime failure; `test_k38_stale_span_replay.py` passes on devel (baseline agrees).
- F25's 14.3 s foreground-fsync symptom and F07's real-quota gate were not re-run; no real-cluster/RDMA/orcd-mini rerun, so K38/K40 remain VERIFY.

## Gate

`EFS_TEST_JOBS=8 scripts/make_test_summary.sh` on intel2 (`~/efs-hk38-gate3`, tree = branch tip merged with devel `62417ac5b`): **TOTAL 560 FAIL 0 NOTRUN 10 STATUS INCOMPLETE (871 s) OVERBUDGET 6**. NOTRUN: RDMA/memcheck tests needing hardware or packages intel2 lacks. INCOMPLETE and OVERBUDGET are the runner's budget flags on a shared host, not failures. An earlier run of the same tree had FAIL 1 (`test_tmpdir_hygiene`: the F25 test hard-coded `/tmp`), fixed by a test-only commit and rerun. Raw output stays on intel2 (`summary.txt`).
