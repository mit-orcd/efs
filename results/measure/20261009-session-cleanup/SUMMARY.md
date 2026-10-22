# SUMMARY — 20261009-session-cleanup

## Purpose and linked item

Gate for the bounded session lease/lock cleanup phase: staged `LEASE_LOCK_NEXT`
(RPC only) and `LEASE_LOCK_BATCH` subcommands that select and log at most eight
exact lease/lock keys under completed global revocation. Linked item: "Bounded
session lease/lock phase (staged)" in `docs/status/dependency-package-20261009.md`
(branch devel-w66, D29/D30 cleanup sequence). Run added by commit `9cec68e14`
("fix(session): add staged exact lease and lock cleanup batches").

## Verdict

FAIL. The full candidate is NOT accepted by this first run: core POSIX passed
212/212 but the two-client peer suite failed 1 of 64
(`peer_append_while_truncate`: client EIO, first-posix2.tsv). All other gates
passed (regressions, unit, live cleanup fixture, WAL crash/reopen, physical
reclamation). Investigation pointed to client read retry ignoring authoritative
GETCHUNKS absence; that defect was fixed and re-gated in the follow-up run
`20261009-truncate-read-absence`.

## Source commit and build identity

- Code commit: `9cec68e14` (adding commit of this run).
- Private build carries `EFS_BUILD_ID/"unknown"`, `EFS_VERSION/"unknown"`,
  build time `2026-10-09T03:49:54Z` (build.log.gz). Installed baseline restored
  afterward: `978caaf7`. No deployed-sha256 file in the run.

## Environment

Private AMD Linux fixture `gc-direct-igaguvk4`, four daemons on one host,
direct I/O, TCP transport, staged pressure enabled only in the fixture.
Installed 978caaf7 direct/TCP service restored with pressure off; no production
session activation.

## Method

Full Linux build and regression suite, final unit/host-decoder/admission/verdict
tests, then a live fixture: bounded lease/lock RPC under a global revocation
gate, exact first-eight replay across a real sync-WAL crash/reopen (13 old
leases: first eight replayed, remaining five drained cold, retained close
marker, other owner/new epoch preserved), final physical reclamation, then core
POSIX and two-client peer POSIX suites. Fixture scripts are the shared
`tests/live` GC/session drivers on AMD; failed client/server logs and stores
were retained on AMD (not in this directory).

## Results

| Gate | Result |
|---|---|
| Full Linux build/regression | PASS |
| Final unit/decoder/admission/verdict tests | PASS |
| WAL crash/reopen fixture (13 leases, exact replay) | PASS |
| Live cleanup RPC + global revocation + physical reclaim | PASS |
| Core POSIX | 212/212 PASS |
| Peer POSIX (first-posix2.tsv) | 63/64 — `peer_append_while_truncate` FAIL (EIO) |

## Limitations and remaining gates

- Candidate rejected; the peer EIO root cause (read retry ignoring authoritative
  GETCHUNKS absence while retaining stale staging mapping) is resolved in
  `20261009-truncate-read-absence`, which supersedes this run's peer result.
- Append cleanup, bounded close effects, receipt retirement and production
  coordinator integration remain open at this checkpoint.
- salt=1 cleanup proves only the lease/lock phase empty, never full cleanup.
- Selector uses fixed memory but can scan unrelated shard keys; no CPU bound.
- Single-host four-daemon check, not hardware power-loss proof.

## Evidence index

| File | sha256 (16) |
|---|---|
| README.md | 1d87d119f3d9c724 |
| session-cleanup.json | 28d80a7d24ba0ced |
| posix.tsv | 296d08f75dd62b67 |
| first-posix2.tsv | f92586823219f0cc |
| first-live.log.gz | 0ebea8b009f0a80e |
| final-unit.log.gz | a55f391c737e294c |
| test.log.gz | 2bb308f23956e21d |
| build.log.gz | b5880c0c745f4d36 |
