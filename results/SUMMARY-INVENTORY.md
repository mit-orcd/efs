# Raw-evidence summary inventory (corrected 2026-10-21, from git index)

Replaces the first scan (34c61ff34), which read a mid-write disk state.
Unit of work: one `SUMMARY.md` per top-level run `results/<area>/<run>/`.

**Totals: 329 runs — adequate 62, thin 135, missing 132**

| Area | runs | adequate | thin | missing |
|---|---|---|---|---|
| io500 | 9 | 0 | 0 | 9 |
| leaks | 1 | 0 | 0 | 1 |
| measure | 290 | 59 | 135 | 96 |
| orcd-ordinary-rdma-direct-20261012 | 2 | 0 | 0 | 2 |
| orcd-v031-rdma-direct-20261012-40f4509d | 1 | 0 | 0 | 1 |
| perf | 2 | 0 | 0 | 2 |
| posix | 16 | 3 | 0 | 13 |
| posix2 | 5 | 0 | 0 | 5 |
| stress | 3 | 0 | 0 | 3 |

## Per-run status

### io500

- `results/io500/20260918-debug-9x1` — missing
- `results/io500/20260920-debug-9x4` — missing
- `results/io500/20260928-150609-rdma` — missing
- `results/io500/20260929-023447-iorperf2` — missing
- `results/io500/20260929-123635-rdma` — missing
- `results/io500/20260930-183504-rdma` — missing
- `results/io500/20261001-074905-rdma` — missing
- `results/io500/20261002-152126-rdma` — missing
- `results/io500/20261002-165956-rdma` — missing
### leaks

- `results/leaks/20260927-035622` — missing
### measure

- `results/measure/20260921-133437-posix-isolated` — missing
- `results/measure/20260921-134109-open-cost` — missing
- `results/measure/20260921-161931-samedir-rate` — adequate
- `results/measure/20260921-162514-ior-hard-scaling` — thin
- `results/measure/20260921-202253-w8-root-srv` — thin
- `results/measure/20260921-203405-w8-root` — adequate
- `results/measure/20260921-215919-w8-stall-timeline` — missing
- `results/measure/20260921-w8-orphans` — missing
- `results/measure/20260922-042153-i17-leader-freeze` — missing
- `results/measure/20260923-025903-i17-leader-freeze` — missing
- `results/measure/20260927-211953-samedir-rate` — missing
- `results/measure/20260927-w11-gate` — missing
- `results/measure/20260927-w9-walk` — missing
- `results/measure/20260928-033420-dd-wall` — missing
- `results/measure/20260928-102121-dd-prof-r2` — missing
- `results/measure/20260928-131651-dd-prof-r2b` — missing
- `results/measure/20260928-134637-dd-prof-r5b` — missing
- `results/measure/20260929-040800-idle-trace` — missing
- `results/measure/20260929-130800-ddposix` — missing
- `results/measure/20260930-032400-g0-wedge` — missing
- `results/measure/20260930-040600-postfix-review` — missing
- `results/measure/20260930-044100-mtime-utimens-close` — missing
- `results/measure/20260930-051000-review2` — missing
- `results/measure/20260930-060000-dd-wedge` — missing
- `results/measure/20260930-063500-perf-dir-review` — missing
- `results/measure/20260930-080000-ecopy-times-review` — missing
- `results/measure/20260930-205300-ecopy-perf-review` — adequate
- `results/measure/20261002-040242-dd16x10g-review` — adequate
- `results/measure/20261002-053311-p0-gates` — adequate
- `results/measure/20261002-054132-p0-x16` — missing
- `results/measure/20261002-060052-p1-d23-w41` — missing
- `results/measure/20261002-134900-w50-gcdbg` — missing
- `results/measure/20261002-143815-w51` — missing
- `results/measure/20261002-151920-posix-ior` — missing
- `results/measure/20261002-165956-redeploy-posix-ior` — missing
- `results/measure/20261002-195156-servers-perf-idle` — missing
- `results/measure/20261005-014440-w23-stalled-compactor` — missing
- `results/measure/20261005-040810-w23-stalled-compactor` — missing
- `results/measure/20261005-045140-p3-benches` — adequate
- `results/measure/20261006-admission-five-rounds` — adequate
- `results/measure/20261006-bench-allocation-multiroot` — adequate
- `results/measure/20261006-bench-cli-move` — adequate
- `results/measure/20261006-bench-profile-harness` — adequate
- `results/measure/20261006-cache-five-rounds` — adequate
- `results/measure/20261006-d25-mtime-coherence` — adequate
- `results/measure/20261006-d25-session-admission` — adequate
- `results/measure/20261006-d25-w60-ten-rounds` — adequate
- `results/measure/20261006-durable-publication` — adequate
- `results/measure/20261006-io-checksum-matrix` — adequate
- `results/measure/20261006-lane-authority` — adequate
- `results/measure/20261006-nuc-correctness` — adequate
- `results/measure/20261006-nuc-ten-rounds` — adequate
- `results/measure/20261006-publication-retirement` — adequate
- `results/measure/20261006-publication-ten-rounds` — adequate
- `results/measure/20261006-writer-five-rounds` — adequate
- `results/measure/20261006-writer-rpc` — adequate
- `results/measure/20261006-writer-token-checkpoint` — adequate
- `results/measure/20261006-xorinox-bench-review` — adequate
- `results/measure/20261006-xorinox-orphan-unlink` — adequate
- `results/measure/20261006-xorinox-second-bench-review` — adequate
- `results/measure/20261007-amd-rdma` — missing
- `results/measure/20261007-bench-hot-path-review` — adequate
- `results/measure/20261007-direct-rollout` — thin
- `results/measure/20261007-gates30` — adequate
- `results/measure/20261007-gc-effects` — missing
- `results/measure/20261007-latency-validation` — thin
- `results/measure/20261007-memory-closure` — thin
- `results/measure/20261007-next-five` — adequate
- `results/measure/20261007-nuc-direct-candidate` — adequate
- `results/measure/20261007-nuc-direct-followup` — adequate
- `results/measure/20261007-nuc-sync-investigation` — adequate
- `results/measure/20261007-roadmap-continuation` — missing
- `results/measure/20261007-roadmap-rounds` — adequate
- `results/measure/20261007-sync-writer-investigation` — thin
- `results/measure/20261007-tcp-five` — thin
- `results/measure/20261007-three-gates` — thin
- `results/measure/20261007-three-package-gates` — adequate
- `results/measure/20261007-w82` — missing
- `results/measure/20261007-w84-envelope` — thin
- `results/measure/20261007-writer-fair-admission` — thin
- `results/measure/20261007-writer-investigation` — adequate
- `results/measure/20261008-amd-compaction-deploy` — missing
- `results/measure/20261008-amd-compaction-pressure` — missing
- `results/measure/20261008-amd-functional` — thin
- `results/measure/20261008-amd-link-symlink` — missing
- `results/measure/20261008-build-warnings` — thin
- `results/measure/20261008-efs-command-options` — missing
- `results/measure/20261008-efs-command-perf` — missing
- `results/measure/20261008-efs-commands` — missing
- `results/measure/20261008-efs-followups` — missing
- `results/measure/20261008-efs-policy` — thin
- `results/measure/20261008-find-stats-merge` — thin
- `results/measure/20261008-help-format` — missing
- `results/measure/20261008-mac-compaction-pressure` — thin
- `results/measure/20261008-nuc-read-waits` — adequate
- `results/measure/20261008-nuc-tiny-create` — thin
- `results/measure/20261008-publication-apply-pending` — thin
- `results/measure/20261008-publication-locators` — thin
- `results/measure/20261008-publication-pending` — thin
- `results/measure/20261008-replay-readiness` — thin
- `results/measure/20261008-v020-amd` — thin
- `results/measure/20261008-w58-negative-replay` — missing
- `results/measure/20261008-w66-mac` — thin
- `results/measure/20261008-w66-readindex` — thin
- `results/measure/20261008-w92-query-catalog` — thin
- `results/measure/20261008-w92-query-foundation` — thin
- `results/measure/20261008-w92-query-publication` — adequate
- `results/measure/20261008-w92-query-rpc` — adequate
- `results/measure/20261008-w93-w96-implementation` — adequate
- `results/measure/20261008-w93-w96-review` — adequate
- `results/measure/20261008-writeback-hour` — thin
- `results/measure/20261008-xorinox-known-bugs` — thin
- `results/measure/20261009-admission-overflow` — thin
- `results/measure/20261009-all-efsd` — thin
- `results/measure/20261009-amd-w73-w64-rxe` — thin
- `results/measure/20261009-append-admission-audit` — thin
- `results/measure/20261009-append-resolve-replay` — thin
- `results/measure/20261009-apply-expansion-audit` — thin
- `results/measure/20261009-atomic-close-locks` — adequate
- `results/measure/20261009-batch-internals` — adequate
- `results/measure/20261009-batch-no-efs` — adequate
- `results/measure/20261009-bounded-edge-retirement` — adequate
- `results/measure/20261009-bounded-gc-cleanup` — adequate
- `results/measure/20261009-bounded-lease-resume` — thin
- `results/measure/20261009-catalog-blocks` — thin
- `results/measure/20261009-catalog-incremental` — missing
- `results/measure/20261009-catalog-index` — thin
- `results/measure/20261009-catalog-ordinal` — missing
- `results/measure/20261009-client-lease-transport` — thin
- `results/measure/20261009-command-pages` — missing
- `results/measure/20261009-deferred-close` — adequate
- `results/measure/20261009-devel-integration` — adequate
- `results/measure/20261009-durability-batching` — adequate
- `results/measure/20261009-durability-tracepoints` — thin
- `results/measure/20261009-find-output-overflow` — missing
- `results/measure/20261009-find-stats-merge` — thin
- `results/measure/20261009-follower-catchup-redirect` — thin
- `results/measure/20261009-full-mount-identity` — thin
- `results/measure/20261009-fuse-lease-retention` — thin
- `results/measure/20261009-fuse-retained-create` — thin
- `results/measure/20261009-hashed-pending-create` — thin
- `results/measure/20261009-lease-ownership-guards` — thin
- `results/measure/20261009-legacy-append-cache-floor` — thin
- `results/measure/20261009-lock-revocation` — thin
- `results/measure/20261009-mac-devel` — thin
- `results/measure/20261009-mac-devel-final` — thin
- `results/measure/20261009-mac-devel-repair` — thin
- `results/measure/20261009-namespace-session-fences` — thin
- `results/measure/20261009-node2-query-cpu` — missing
- `results/measure/20261009-nuc-publication` — thin
- `results/measure/20261009-nuc-w73-w64` — thin
- `results/measure/20261009-pending-create-wire` — thin
- `results/measure/20261009-pending-work-charge` — thin
- `results/measure/20261009-profile-repeat` — thin
- `results/measure/20261009-publication-durability` — thin
- `results/measure/20261009-put-ticket-transport` — thin
- `results/measure/20261009-query-clock` — thin
- `results/measure/20261009-registered-append-revocation` — thin
- `results/measure/20261009-report-effects` — thin
- `results/measure/20261009-report-log-coverage` — thin
- `results/measure/20261009-report-session` — thin
- `results/measure/20261009-retained-publication` — thin
- `results/measure/20261009-retained-sequenced-lease` — thin
- `results/measure/20261009-revoked-append-retirement` — thin
- `results/measure/20261009-revoked-client-identities` — thin
- `results/measure/20261009-sequenced-lease-handoff` — thin
- `results/measure/20261009-session-append-cleanup` — adequate
- `results/measure/20261009-session-cleanup` — adequate
- `results/measure/20261009-tiny-five-minutes` — thin
- `results/measure/20261009-transaction-durability` — thin
- `results/measure/20261009-truncate-read-absence` — thin
- `results/measure/20261009-w60-amd-tree` — thin
- `results/measure/20261009-w66-profiles` — thin
- `results/measure/20261009-w84-closure` — thin
- `results/measure/20261009-w92-clean-cluster` — thin
- `results/measure/20261009-w92-merged-three-rounds` — thin
- `results/measure/20261009-w92-query-disk` — thin
- `results/measure/20261009-w92-three-rounds` — thin
- `results/measure/20261009-w92-tiny-profile` — thin
- `results/measure/20261009-w93-batch-hour` — thin
- `results/measure/20261009-w93-batch-hour-v2` — thin
- `results/measure/20261009-w93-overlap-hour` — thin
- `results/measure/20261009-w93-read-pressure-hour` — thin
- `results/measure/20261009-w97-clean-baseline` — thin
- `results/measure/20261009-w97-flow-enhanced` — missing
- `results/measure/20261009-w97-flow-repeats` — missing
- `results/measure/20261009-w97-flow-repeats-v2` — thin
- `results/measure/20261009-w97-flow-smoke` — thin
- `results/measure/20261009-w97-group-isolation` — thin
- `results/measure/20261009-w97-partial-reply` — thin
- `results/measure/20261009-w97-pump-phases` — adequate
- `results/measure/20261009-w97-replication-baseline` — thin
- `results/measure/20261009-w97-restored` — thin
- `results/measure/20261009-writeback-final-gates` — thin
- `results/measure/20261009-xorinox-w55-stress` — thin
- `results/measure/20261009-xorinox-w58-w55` — thin
- `results/measure/20261010-amd-three-hour` — adequate
- `results/measure/20261010-devel-nuc` — adequate
- `results/measure/20261010-mac-integration-2` — thin
- `results/measure/20261010-mac-refresh` — thin
- `results/measure/20261010-merged-ticket-storage-fence` — thin
- `results/measure/20261010-ticketed-put-hour` — thin
- `results/measure/20261010-ticketed-writeback` — thin
- `results/measure/20261010-w66-foundation` — thin
- `results/measure/20261010-w66-leadership` — thin
- `results/measure/20261010-w92-closure` — missing
- `results/measure/20261010-w92-incremental` — thin
- `results/measure/20261010-w92-next-three-rounds` — thin
- `results/measure/20261010-w93-checkpoint-hour` — thin
- `results/measure/20261010-w93-closure` — thin
- `results/measure/20261010-w93-integrated` — thin
- `results/measure/20261010-w97-latest-nvme` — thin
- `results/measure/20261010-w97-three-drive` — thin
- `results/measure/20261011-amd-all-suites` — missing
- `results/measure/20261011-amd-feedback` — missing
- `results/measure/20261011-amd-feedback-final` — missing
- `results/measure/20261011-amd-integration` — missing
- `results/measure/20261011-amd-overload` — missing
- `results/measure/20261011-amd-sustained` — missing
- `results/measure/20261011-devel-nuc` — thin
- `results/measure/20261011-intel-buffered-28b2089b` — adequate
- `results/measure/20261011-intel-devel-intel` — adequate
- `results/measure/20261011-mac-query-delta` — missing
- `results/measure/20261011-nuc-resume` — thin
- `results/measure/20261011-orcd-branch-integration` — missing
- `results/measure/20261011-orcd-communication` — missing
- `results/measure/20261011-orcd-integration` — missing
- `results/measure/20261011-orcd-merged-rollout` — thin
- `results/measure/20261011-orcd-query-ab` — thin
- `results/measure/20261011-orcd-rollout` — missing
- `results/measure/20261011-v030-feedback-jobs16` — missing
- `results/measure/20261011-v030-merged-native` — missing
- `results/measure/20261011-v030-ordinary-control` — missing
- `results/measure/20261011-v030-release-integration` — missing
- `results/measure/20261012-common03bd-amd` — thin
- `results/measure/20261012-common2bf-amd` — thin
- `results/measure/20261012-common52-amd` — thin
- `results/measure/20261012-common52-amd-quiet` — thin
- `results/measure/20261012-commondef-amd` — thin
- `results/measure/20261012-commone59-amd-native` — thin
- `results/measure/20261012-commone59-amd-quiet` — missing
- `results/measure/20261012-commonecdc-amd-diagnostic` — missing
- `results/measure/20261012-commonecdc-amd-isolated` — missing
- `results/measure/20261012-commonecdc-amd-native` — thin
- `results/measure/20261012-commonecdc-amd-quiet` — thin
- `results/measure/20261012-deep-directory-fix` — thin
- `results/measure/20261012-evidence-cleanup` — thin
- `results/measure/20261012-final-1e3a4638` — thin
- `results/measure/20261012-intel-common03` — thin
- `results/measure/20261012-intel-common2bf-prerequisite` — thin
- `results/measure/20261012-intel-common52` — thin
- `results/measure/20261012-intel-commondef` — thin
- `results/measure/20261012-intel-e59-directory-diagnostic` — thin
- `results/measure/20261012-intel-e59-directory-worker-diagnostic` — thin
- `results/measure/20261012-intel-e59-focused` — thin
- `results/measure/20261012-intel-e59-rpc-diagnostic` — thin
- `results/measure/20261012-intel-ecdc-focused` — thin
- `results/measure/20261012-intel-read-completion` — thin
- `results/measure/20261012-leftover-evidence-review` — thin
- `results/measure/20261012-nuc-03bd-group3` — missing
- `results/measure/20261012-nuc-28-qualification` — missing
- `results/measure/20261012-nuc-2bf-group3` — missing
- `results/measure/20261012-nuc-52-group3` — adequate
- `results/measure/20261012-nuc-52-seven` — missing
- `results/measure/20261012-nuc-b239-qualification` — adequate
- `results/measure/20261012-nuc-c2-qualification` — missing
- `results/measure/20261012-nuc-def-group3` — missing
- `results/measure/20261012-nuc-e59-focused` — missing
- `results/measure/20261012-nuc-ecdc-focused` — missing
- `results/measure/20261012-nuc-manager-handoff` — adequate
- `results/measure/20261012-orcd-default-repair` — missing
- `results/measure/20261012-orcd-release-rdma-direct` — missing
- `results/measure/20261012-orcd-standalone-fault-fix` — missing
- `results/measure/20261012-orcd-valgrind-fix` — missing
- `results/measure/20261012-release-native` — missing
- `results/measure/20261012-repair-journal` — missing
- `results/measure/20261012-repair-review` — missing
- `results/measure/20261012-v030-create-single-hook` — missing
- `results/measure/20261012-v030-cut-retry-local` — missing
- `results/measure/20261012-v030-defect-only-inverse` — missing
- `results/measure/20261012-v030-merged-native` — missing
- `results/measure/20261012-v030-mixed-cut-oracle-isolation` — missing
- `results/measure/20261012-v030-ordinary-overwrite` — missing
- `results/measure/20261012-v030-overwrite-qualified` — missing
- `results/measure/20261012-v030-phase-diagnostic-local` — missing
- `results/measure/20261012-v030-phase-work` — missing
- `results/measure/20261012-v030-query-stage-diagnostic` — missing
- `results/measure/20261012-v031-15e2a531` — thin
- `results/measure/20261012-v031-buffered-15e2a531` — thin
- `results/measure/20261012-w112-overwrite` — missing
### orcd-ordinary-rdma-direct-20261012

- `results/orcd-ordinary-rdma-direct-20261012/first-f7e8475e` — missing
- `results/orcd-ordinary-rdma-direct-20261012/remote` — missing
### orcd-v031-rdma-direct-20261012-40f4509d

- `results/orcd-v031-rdma-direct-20261012-40f4509d/remote` — missing
### perf

- `results/perf/20260918-w4-honest` — missing
- `results/perf/20260918-w4-honest4` — missing
### posix

- `results/posix/20260917-191430` — missing
- `results/posix/20260922-020950` — missing
- `results/posix/20260926-0330-diag` — missing
- `results/posix/20260927-015719` — adequate
- `results/posix/20260927-033723` — missing
- `results/posix/20260927-044348` — missing
- `results/posix/20260927-123717` — missing
- `results/posix/20260928-033823` — missing
- `results/posix/20260928-034049` — missing
- `results/posix/20260928-043918` — missing
- `results/posix/20261001-154333` — missing
- `results/posix/20261001-155455` — missing
- `results/posix/20261001-160049` — missing
- `results/posix/20261002-130142` — missing
- `results/posix/20261002-151947` — adequate
- `results/posix/20261002-170119` — adequate
### posix2

- `results/posix2/20260927-123946` — missing
- `results/posix2/20260927-190509` — missing
- `results/posix2/20260928-034350` — missing
- `results/posix2/20261001-073048` — missing
- `results/posix2/20261002-152031` — missing
### stress

- `results/stress/20260918-n1-w1` — missing
- `results/stress/20260918-w2` — missing
- `results/stress/truncate-big-20261002-012442` — missing
