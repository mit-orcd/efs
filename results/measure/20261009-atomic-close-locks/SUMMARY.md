# SUMMARY — 20261009-atomic-close-locks

## Purpose and linked item

Validation run for atomic, capped last-close lock cleanup: lock grants cap each
of two domains at 32 rows; final-close cleanup validates that envelope and
performs one atomic batch of at most 64 deletes, failing closed on invalid or
over-cap records. Linked to the dependency-linked correctness package of
2026-10-09 (`docs/status/dependency-package-20261009.md`); added to the
raw-evidence branch by commit `2830a709f` ("fix(leases): make final-close lock
cleanup bounded and atomic").

## Verdict

PASS for the lock-side loop/atomicity gap — all new lock assertions pass on the
fixed source (the old source fails 6 assertions, retained as a baseline
counter-example), full AMD Linux regression passes, and the private AMD
direct/TCP fixture passes core POSIX 212/212, peer 64/64, bounded session
cleanup, guarded-lease scenarios, and final physical inventory/quota zero with
late PUT fenced.

Not a full-package acceptance: a retained second fixture
(`gc-direct-nvqc6dgf`) hit BUSY from the legacy management fence with staged
pressure enabled (legacy LEASE_DROP carries UINT64_MAX charge by design);
pressure-enabled legacy revocation is explicitly not certified.

## Source commit and build identity

- Candidate daemon (`efsd`) SHA256:
  `f55334381e775dd0cb720ed1d641a5862acec7a84cae83a455b0252462d05233`
  (also `efs-fuse` and `efs-mgmt` SHA256s in `manifest.json`).
- Source-tree SHA256 `4b76f6f2010b93…` and test-script SHA256 in
  `manifest.json`. Git commit id not recorded in the run.

## Environment

- Hosts: AMD cluster (`amd-395` per posix.tsv header), private fixture
  `gc-direct-x1nxvbaq` (n1/n2) and `gc-direct-3hsh0vpr` (n3/n4) under
  `/data1/efs/evidence/w66-gc-fixtures/`, port 28940.
- Topology: 4 daemons, direct I/O, TCP transport, single storage root per
  daemon; mounts `/data1/efs/mnt` and `/data1/efs/mnt2`.
- Pressure disabled (as in installed production); enabled gates per
  `manifest.json`: session_cleanup, guarded_leases, append_audit, posix.

## Method

Private-fixture harness (same family as `tests/live/…` scripts; exact driver
not stored in the run — `manifest.json` records its SHA256). Steps: build
candidate with the lock fix; run old-vs-new lock unit assertions
(`efs-lock-before-test.log` = old, `efs-close-tests.log` = fixed full
regression); run the actual host-function close-effects test; bring up the
4-daemon direct/TCP fixture; run bounded session cleanup stages (prepare /
first / finish per `session-cleanup.json`), guarded lease reorder/lost
reply/probe/cold/last-close scenarios, append/fold/GC, then core POSIX and peer
suites; finish with restart, physical-fragment inventory, quota-zero and
late-PUT fencing checks; restore both installed AMD mounts.

## Results

| Check | Result |
|---|---|
| Old lock source, new assertions | 6 failures (expected counter-example) |
| Fixed source, full Linux regression | pass (`efs-close-tests.log`, all OK) |
| Close-effects host-function test | PASS (BUSY persists obligation, IO retains, resolution drains) |
| Bounded session cleanup (prepare/first/finish) | PASS each stage; active cleanup refused as designed |
| Core POSIX suite | 212/212 pass, 16.2 s |
| Peer suite | 64/64 pass, 13.8 s |
| Post-restart inventory / quota / late PUT | empty inventory, quota zero, PUT fenced |

## Limitations and remaining gates

- Closes only the lock-side loop/atomicity gap, not the whole dependency
  package. Remaining per the run README: legacy append drain, bounded
  successor/reopen serialization, session append cleanup, receipt retirement,
  production lifecycle integration.
- Pressure-enabled legacy revocation not certified (BUSY from legacy fence in
  retained fixture `gc-direct-nvqc6dgf` is by-design behavior).

## Evidence index

| File | sha256 (16) |
|---|---|
| README.md | bc259f2c80b6c87c |
| posix.tsv | 03f231546ca4cb49 |
| posix2.tsv | 2f0245e2ed9e6598 |
| manifest.json | 6d96202cce38ab26 |
| session-cleanup.json | b86747896f995151 |
| efs-close-lock-amd-live.log | cd2ceade74b9b07d |
| efs-close-effects-test.log | c092ce9ac3a2c9e9 |
| efs-lock-before-test.log | 85929ea88700ecd2 |
| efs-close-tests.log | 85ddda276a7997ff |

**Raw evidence note (2026-10-23):** the bulk data in `w66-gc-fixtures` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
