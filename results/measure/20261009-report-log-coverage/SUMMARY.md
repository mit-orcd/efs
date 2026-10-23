# SUMMARY — 20261009-report-log-coverage

## Purpose and linked item

Gate for the REPORT shared-log coverage and overflow-lane fixes: REPORT waits
for the shared log boundary before advertising publication entries as durable,
propagates hold/sync failures, returns INVAL (not whole-batch OK) for an
invalid record, and activates overflow lanes (records beyond the 32-inode
cache) before taking the REPORT log hold. Part of the 2026-10-09
dependency-linked correctness package
(`docs/status/dependency-package-20261009.md`, section "REPORT shared-log
coverage and overflow lanes — implemented/gated"). Raw evidence added by
commits `b6e973cf0` and `aea76bbe2`.

## Verdict

PASS. All four private AMD fixtures complete with every gate line PASS;
`live.log` ends with "GC reclamation direct PASS" after restart quota-zero and
late-PUT fencing, and `regressions.log` completes the full code regression
recipes (POSIX suite 212/212, peer 64/64 recorded inside the live gates).
One recorded caveat, kept visible per README: the strengthened invalid-record
check exposed that a previous live REPORT retry proof had submitted a sparse
zero-node base and the old skip behavior returned OK; that mapping-retry claim
is superseded here by a replay of the actual nonzero live span descriptor plus
an explicit INVAL requirement for a zero-node record.

## Source commit and build identity

- Fix commits named in the adding commits: `b6e973cf0` (durable log coverage
  and overflow lanes), `aea76bbe2` (invalid batch tails after accepted
  prefixes).
- `build.log` records a private Linux build with `EFS_BUILD_TIME
  2026-10-09T22:54:35Z`; `EFS_BUILD_ID`/`EFS_VERSION`/`EFS_GIT_BRANCH` are
  "unknown" in this private build. No deployed-version.txt or binary hashes in
  the run.

## Environment

- AMD host (amd-395 per suite headers elsewhere in this batch), four private
  efsd daemons on one host, direct I/O, TCP transport, FUSE client, under
  `/data1/efs/evidence/lease-retention-fixtures/`.
- Fixture ids: `gc-direct-fvkizk15` (live.log), `gc-direct-gyil3xsd`
  (mount-sync.log), `gc-direct-cwx12oso` (sync-write.log),
  `gc-direct-txewwmg0` (partial-prefix.log). Installed 978caaf7 service
  restored afterward per README.

## Method

Live gates driven by the repo live harness (`tests/live/gc_reclamation.py`
family in the private source tree) plus extracted production-helper
regressions; full code regressions via `make test` (`regressions.log`), Linux
build via `build.log`. Coverage: 40 distinct fresh cross-group lanes in one
REPORT with exact sparse/cold bytes and physical reclamation; nested
shared-log hold blocking proof; sequential O_SYNC/O_DSYNC small/queued/append
writes and mount-wide sync visible on peer before close; append/fsync
boundaries and 800-record append audit; final focused fixture submits a valid
publication prefix followed by an invalid zero-node tail, requires INVAL,
verifies the prefix committed (FileID, generation, fragment nodes, checksums),
then passes the full valid 40-record retry. Runner restores the installed
service on exit.

## Results

| Gate | Result |
|---|---|
| 40-lane REPORT, cold bytes, physical reclaim (gc-direct-fvkizk15) | PASS |
| Nested shared-log hold: release waiter blocked until other owner releases | PASS |
| Sync-write fixtures (O_SYNC/O_DSYNC, mount-wide sync) | PASS |
| Invalid zero-node tail after valid prefix | INVAL returned, prefix verified committed; 40-record retry PASS |
| Core POSIX / peer POSIX inside fixtures | 212/212, 64/64 |
| Full code regressions | PASS |
| Linux build | warning-free |

## Limitations and remaining gates

- Does not close aggregate partial-publication replay outcomes and does not
  activate D25; no durable per-record result reconciliation.
- Single-host four-daemon proof; no hardware power-loss or RDMA claim.
- Candidate committed but not installed over the restored service.

## Evidence index

| File | sha256 (16) |
|---|---|
| README.md | c4db361c39645a6c |
| live.log (gc-direct-fvkizk15, main gate) | e963cf0545fa04ea |
| partial-prefix.log (gc-direct-txewwmg0, INVAL/prefix gate) | af2df377b4eac1c9 |
| sync-write.log (gc-direct-cwx12oso) | 27335fc9a0afe4ea |
| mount-sync.log (gc-direct-gyil3xsd) | 94fdc9d41fefeee5 |
| regressions.log (full make test) | f894d3c66b4ed254 |
| build.log (warning-free Linux build) | 7fa447fd00ea8d2f |

**Raw evidence note (2026-10-23):** the bulk data in `lease-retention-fixtures` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
