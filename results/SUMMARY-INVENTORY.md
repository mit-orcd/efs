# Raw-evidence summary inventory (generated 2026-10-21, read-only scan)

Unit of work: one `SUMMARY.md` per top-level run directory
`results/<area>/<run>/`, covering its nested phases in the evidence index.
Status: adequate = SUMMARY.md with verdict + >=30 lines; thin = SUMMARY.md
present but short, or only a raw-era README.md; missing = neither.

**Totals: 329 runs — adequate 20, thin 157, missing 152**

| Area | runs | adequate | thin | missing |
|---|---|---|---|---|
| io500 | 9 | 0 | 0 | 9 |
| leaks | 1 | 0 | 0 | 1 |
| measure | 290 | 20 | 157 | 113 |
| orcd-ordinary-rdma-direct-20261012 | 2 | 0 | 0 | 2 |
| orcd-v031-rdma-direct-20261012-40f4509d | 1 | 0 | 0 | 1 |
| perf | 2 | 0 | 0 | 2 |
| posix | 16 | 0 | 0 | 16 |
| posix2 | 5 | 0 | 0 | 5 |
| stress | 3 | 0 | 0 | 3 |

Data files outside results/: docs/status/evidence (10), docs/archive (2),
docs/reviews (3), docs/status JSON (13).

## Per-run status

### io500

- `20260918-debug-9x1` — missing

- `20260920-debug-9x4` — missing

- `20260928-150609-rdma` — missing

- `20260929-023447-iorperf2` — missing

- `20260929-123635-rdma` — missing

- `20260930-183504-rdma` — missing

- `20261001-074905-rdma` — missing

- `20261002-152126-rdma` — missing

- `20261002-165956-rdma` — missing

### leaks

- `20260927-035622` — missing

### measure

- `20260921-133437-posix-isolated` — missing

- `20260921-134109-open-cost` — missing

- `20260921-161931-samedir-rate` — missing

- `20260921-162514-ior-hard-scaling` — missing

- `20260921-202253-w8-root-srv` — missing

- `20260921-203405-w8-root` — missing

- `20260921-215919-w8-stall-timeline` — missing

- `20260921-w8-orphans` — missing

- `20260922-042153-i17-leader-freeze` — missing

- `20260923-025903-i17-leader-freeze` — missing

- `20260927-211953-samedir-rate` — missing

- `20260927-w11-gate` — missing

- `20260927-w9-walk` — missing

- `20260928-033420-dd-wall` — missing

- `20260928-102121-dd-prof-r2` — missing

- `20260928-131651-dd-prof-r2b` — missing

- `20260928-134637-dd-prof-r5b` — missing

- `20260929-040800-idle-trace` — missing

- `20260929-130800-ddposix` — missing

- `20260930-032400-g0-wedge` — missing

- `20260930-040600-postfix-review` — missing

- `20260930-044100-mtime-utimens-close` — missing

- `20260930-051000-review2` — missing

- `20260930-060000-dd-wedge` — missing

- `20260930-063500-perf-dir-review` — missing

- `20260930-080000-ecopy-times-review` — missing

- `20260930-205300-ecopy-perf-review` — missing

- `20261002-040242-dd16x10g-review` — missing

- `20261002-053311-p0-gates` — missing

- `20261002-054132-p0-x16` — missing

- `20261002-060052-p1-d23-w41` — missing

- `20261002-134900-w50-gcdbg` — missing

- `20261002-143815-w51` — missing

- `20261002-151920-posix-ior` — missing

- `20261002-165956-redeploy-posix-ior` — missing

- `20261002-195156-servers-perf-idle` — missing

- `20261005-014440-w23-stalled-compactor` — missing

- `20261005-040810-w23-stalled-compactor` — missing

- `20261005-045140-p3-benches` — missing

- `20261006-admission-five-rounds` — adequate

- `20261006-bench-allocation-multiroot` — adequate

- `20261006-bench-cli-move` — adequate

- `20261006-bench-profile-harness` — adequate

- `20261006-cache-five-rounds` — adequate

- `20261006-d25-mtime-coherence` — adequate

- `20261006-d25-session-admission` — thin

- `20261006-d25-w60-ten-rounds` — adequate

- `20261006-durable-publication` — adequate

- `20261006-io-checksum-matrix` — adequate

- `20261006-lane-authority` — adequate

- `20261006-nuc-correctness` — adequate

- `20261006-nuc-ten-rounds` — adequate

- `20261006-publication-retirement` — adequate

- `20261006-publication-ten-rounds` — adequate

- `20261006-writer-five-rounds` — thin

- `20261006-writer-rpc` — thin

- `20261006-writer-token-checkpoint` — thin

- `20261006-xorinox-bench-review` — missing

- `20261006-xorinox-orphan-unlink` — adequate

- `20261006-xorinox-second-bench-review` — missing

- `20261007-amd-rdma` — missing

- `20261007-bench-hot-path-review` — adequate

- `20261007-direct-rollout` — thin

- `20261007-gates30` — adequate

- `20261007-gc-effects` — missing

- `20261007-latency-validation` — thin

- `20261007-memory-closure` — thin

- `20261007-next-five` — thin

- `20261007-nuc-direct-candidate` — thin

- `20261007-nuc-direct-followup` — thin

- `20261007-nuc-sync-investigation` — thin

- `20261007-roadmap-continuation` — missing

- `20261007-roadmap-rounds` — adequate

- `20261007-sync-writer-investigation` — thin

- `20261007-tcp-five` — thin

- `20261007-three-gates` — thin

- `20261007-three-package-gates` — adequate

- `20261007-w82` — missing

- `20261007-w84-envelope` — thin

- `20261007-writer-fair-admission` — thin

- `20261007-writer-investigation` — adequate

- `20261008-amd-compaction-deploy` — missing

- `20261008-amd-compaction-pressure` — missing

- `20261008-amd-functional` — thin

- `20261008-amd-link-symlink` — missing

- `20261008-build-warnings` — thin

- `20261008-efs-command-options` — missing

- `20261008-efs-command-perf` — missing

- `20261008-efs-commands` — missing

- `20261008-efs-followups` — missing

- `20261008-efs-policy` — thin

- `20261008-find-stats-merge` — thin

- `20261008-help-format` — missing

- `20261008-mac-compaction-pressure` — thin

- `20261008-nuc-read-waits` — thin

- `20261008-nuc-tiny-create` — thin

- `20261008-publication-apply-pending` — thin

- `20261008-publication-locators` — thin

- `20261008-publication-pending` — thin

- `20261008-replay-readiness` — thin

- `20261008-v020-amd` — thin

- `20261008-w58-negative-replay` — missing

- `20261008-w66-mac` — thin

- `20261008-w66-readindex` — thin

- `20261008-w92-query-catalog` — thin

- `20261008-w92-query-foundation` — thin

- `20261008-w92-query-publication` — thin

- `20261008-w92-query-rpc` — thin

- `20261008-w93-w96-implementation` — missing

- `20261008-w93-w96-review` — thin

- `20261008-writeback-hour` — thin

- `20261008-xorinox-known-bugs` — thin

- `20261009-admission-overflow` — thin

- `20261009-all-efsd` — thin

- `20261009-amd-w73-w64-rxe` — thin

- `20261009-append-admission-audit` — thin

- `20261009-append-resolve-replay` — thin

- `20261009-apply-expansion-audit` — thin

- `20261009-atomic-close-locks` — thin

- `20261009-batch-internals` — thin

- `20261009-batch-no-efs` — thin

- `20261009-bounded-edge-retirement` — thin

- `20261009-bounded-gc-cleanup` — thin

- `20261009-bounded-lease-resume` — thin

- `20261009-catalog-blocks` — thin

- `20261009-catalog-incremental` — missing

- `20261009-catalog-index` — thin

- `20261009-catalog-ordinal` — missing

- `20261009-client-lease-transport` — thin

- `20261009-command-pages` — missing

- `20261009-deferred-close` — thin

- `20261009-devel-integration` — thin

- `20261009-durability-batching` — thin

- `20261009-durability-tracepoints` — thin

- `20261009-find-output-overflow` — missing

- `20261009-find-stats-merge` — thin

- `20261009-follower-catchup-redirect` — thin

- `20261009-full-mount-identity` — thin

- `20261009-fuse-lease-retention` — thin

- `20261009-fuse-retained-create` — thin

- `20261009-hashed-pending-create` — thin

- `20261009-lease-ownership-guards` — thin

- `20261009-legacy-append-cache-floor` — thin

- `20261009-lock-revocation` — thin

- `20261009-mac-devel` — thin

- `20261009-mac-devel-final` — thin

- `20261009-mac-devel-repair` — thin

- `20261009-namespace-session-fences` — thin

- `20261009-node2-query-cpu` — missing

- `20261009-nuc-publication` — thin

- `20261009-nuc-w73-w64` — thin

- `20261009-pending-create-wire` — thin

- `20261009-pending-work-charge` — thin

- `20261009-profile-repeat` — thin

- `20261009-publication-durability` — thin

- `20261009-put-ticket-transport` — thin

- `20261009-query-clock` — thin

- `20261009-registered-append-revocation` — thin

- `20261009-report-effects` — thin

- `20261009-report-log-coverage` — thin

- `20261009-report-session` — thin

- `20261009-retained-publication` — thin

- `20261009-retained-sequenced-lease` — thin

- `20261009-revoked-append-retirement` — thin

- `20261009-revoked-client-identities` — thin

- `20261009-sequenced-lease-handoff` — thin

- `20261009-session-append-cleanup` — thin

- `20261009-session-cleanup` — thin

- `20261009-tiny-five-minutes` — thin

- `20261009-transaction-durability` — thin

- `20261009-truncate-read-absence` — thin

- `20261009-w60-amd-tree` — thin

- `20261009-w66-profiles` — thin

- `20261009-w84-closure` — thin

- `20261009-w92-clean-cluster` — thin

- `20261009-w92-merged-three-rounds` — thin

- `20261009-w92-query-disk` — thin

- `20261009-w92-three-rounds` — thin

- `20261009-w92-tiny-profile` — thin

- `20261009-w93-batch-hour` — thin

- `20261009-w93-batch-hour-v2` — thin

- `20261009-w93-overlap-hour` — thin

- `20261009-w93-read-pressure-hour` — thin

- `20261009-w97-clean-baseline` — thin

- `20261009-w97-flow-enhanced` — missing

- `20261009-w97-flow-repeats` — missing

- `20261009-w97-flow-repeats-v2` — thin

- `20261009-w97-flow-smoke` — thin

- `20261009-w97-group-isolation` — thin

- `20261009-w97-partial-reply` — thin

- `20261009-w97-pump-phases` — thin

- `20261009-w97-replication-baseline` — thin

- `20261009-w97-restored` — thin

- `20261009-writeback-final-gates` — thin

- `20261009-xorinox-w55-stress` — thin

- `20261009-xorinox-w58-w55` — thin

- `20261010-amd-three-hour` — thin

- `20261010-devel-nuc` — missing

- `20261010-mac-integration-2` — thin

- `20261010-mac-refresh` — thin

- `20261010-merged-ticket-storage-fence` — thin

- `20261010-ticketed-put-hour` — thin

- `20261010-ticketed-writeback` — thin

- `20261010-w66-foundation` — thin

- `20261010-w66-leadership` — thin

- `20261010-w92-closure` — missing

- `20261010-w92-incremental` — thin

- `20261010-w92-next-three-rounds` — thin

- `20261010-w93-checkpoint-hour` — thin

- `20261010-w93-closure` — thin

- `20261010-w93-integrated` — thin

- `20261010-w97-latest-nvme` — thin

- `20261010-w97-three-drive` — thin

- `20261011-amd-all-suites` — missing

- `20261011-amd-feedback` — missing

- `20261011-amd-feedback-final` — missing

- `20261011-amd-integration` — missing

- `20261011-amd-overload` — missing

- `20261011-amd-sustained` — missing

- `20261011-devel-nuc` — thin

- `20261011-intel-buffered-28b2089b` — missing

- `20261011-intel-devel-intel` — missing

- `20261011-mac-query-delta` — missing

- `20261011-nuc-resume` — thin

- `20261011-orcd-branch-integration` — missing

- `20261011-orcd-communication` — missing

- `20261011-orcd-integration` — missing

- `20261011-orcd-merged-rollout` — thin

- `20261011-orcd-query-ab` — thin

- `20261011-orcd-rollout` — missing

- `20261011-v030-feedback-jobs16` — missing

- `20261011-v030-merged-native` — missing

- `20261011-v030-ordinary-control` — missing

- `20261011-v030-release-integration` — missing

- `20261012-common03bd-amd` — thin

- `20261012-common2bf-amd` — thin

- `20261012-common52-amd` — thin

- `20261012-common52-amd-quiet` — thin

- `20261012-commondef-amd` — thin

- `20261012-commone59-amd-native` — thin

- `20261012-commone59-amd-quiet` — missing

- `20261012-commonecdc-amd-diagnostic` — missing

- `20261012-commonecdc-amd-isolated` — missing

- `20261012-commonecdc-amd-native` — thin

- `20261012-commonecdc-amd-quiet` — thin

- `20261012-deep-directory-fix` — thin

- `20261012-evidence-cleanup` — thin

- `20261012-final-1e3a4638` — thin

- `20261012-intel-common03` — thin

- `20261012-intel-common2bf-prerequisite` — thin

- `20261012-intel-common52` — thin

- `20261012-intel-commondef` — thin

- `20261012-intel-e59-directory-diagnostic` — thin

- `20261012-intel-e59-directory-worker-diagnostic` — thin

- `20261012-intel-e59-focused` — thin

- `20261012-intel-e59-rpc-diagnostic` — thin

- `20261012-intel-ecdc-focused` — thin

- `20261012-intel-read-completion` — thin

- `20261012-leftover-evidence-review` — thin

- `20261012-nuc-03bd-group3` — missing

- `20261012-nuc-28-qualification` — missing

- `20261012-nuc-2bf-group3` — missing

- `20261012-nuc-52-group3` — missing

- `20261012-nuc-52-seven` — missing

- `20261012-nuc-b239-qualification` — missing

- `20261012-nuc-c2-qualification` — missing

- `20261012-nuc-def-group3` — missing

- `20261012-nuc-e59-focused` — missing

- `20261012-nuc-ecdc-focused` — missing

- `20261012-nuc-manager-handoff` — missing

- `20261012-orcd-default-repair` — missing

- `20261012-orcd-release-rdma-direct` — missing

- `20261012-orcd-standalone-fault-fix` — missing

- `20261012-orcd-valgrind-fix` — missing

- `20261012-release-native` — missing

- `20261012-repair-journal` — missing

- `20261012-repair-review` — missing

- `20261012-v030-create-single-hook` — missing

- `20261012-v030-cut-retry-local` — missing

- `20261012-v030-defect-only-inverse` — missing

- `20261012-v030-merged-native` — missing

- `20261012-v030-mixed-cut-oracle-isolation` — missing

- `20261012-v030-ordinary-overwrite` — missing

- `20261012-v030-overwrite-qualified` — missing

- `20261012-v030-phase-diagnostic-local` — missing

- `20261012-v030-phase-work` — missing

- `20261012-v030-query-stage-diagnostic` — missing

- `20261012-v031-15e2a531` — thin

- `20261012-v031-buffered-15e2a531` — thin

- `20261012-w112-overwrite` — missing

### orcd-ordinary-rdma-direct-20261012

- `first-f7e8475e` — missing

- `remote` — missing

### orcd-v031-rdma-direct-20261012-40f4509d

- `remote` — missing

### perf

- `20260918-w4-honest` — missing

- `20260918-w4-honest4` — missing

### posix

- `20260917-191430` — missing

- `20260922-020950` — missing

- `20260926-0330-diag` — missing

- `20260927-015719` — missing

- `20260927-033723` — missing

- `20260927-044348` — missing

- `20260927-123717` — missing

- `20260928-033823` — missing

- `20260928-034049` — missing

- `20260928-043918` — missing

- `20261001-154333` — missing

- `20261001-155455` — missing

- `20261001-160049` — missing

- `20261002-130142` — missing

- `20261002-151947` — missing

- `20261002-170119` — missing

### posix2

- `20260927-123946` — missing

- `20260927-190509` — missing

- `20260928-034350` — missing

- `20261001-073048` — missing

- `20261002-152031` — missing

### stress

- `20260918-n1-w1` — missing

- `20260918-w2` — missing

- `truncate-big-20261002-012442` — missing

