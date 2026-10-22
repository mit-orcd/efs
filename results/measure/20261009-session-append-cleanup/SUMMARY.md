# SUMMARY — 20261009-session-append-cleanup

## Purpose and linked item

Gate for the bounded revoked-session append cleanup phase: RPC-only
`SESSION_APPEND_NEXT` plus `raft-session NODE:PORT cleanup-appends UUID OLD_EPOCH
SHARD` resolve one exact old-session OPEN reservation per transaction. Linked
item: "Bounded revoked-session append phase" in
`docs/status/dependency-package-20261009.md` (branch devel-w66, D29/D30 cleanup
sequence). Run added by commit `28d941824` ("feat(sessions): recover revoked
append reservations in bounded transactions").

## Verdict

PASS. The live fixture resolved all 64 OPEN reservations in two batches of 32
across a full-cluster crash, offline inspection, leader-loss redirect and exact
replay; unit, Linux regression, core POSIX 212/212, peer 64/64 and cold
GC/restart gates all pass in the raw logs. Scope is limited to the
OPEN-reservation phase only.

## Source commit and build identity

- Code commit: `28d941824` (adding commit of this run).
- Daemon SHA256 `ef9534be3a13bb0f125e0aceea0b8063a080828695bae652cd021a4789664870`,
  management SHA256 `a4d2947fb40d637aacac6ab5f9c10e38d7f75c2081e165cbacdba98a5231d781`
  (recorded in README.md).

## Environment

Private AMD Linux fixture `gc-direct-n4s969j6`, port 29340, four daemons on one
host, direct I/O, TCP transport, staged pressure policy enabled only inside the
fixture. Installed direct/TCP service restored with pressure off afterward.

## Method

Fixture creates 64 x 128 KiB OPEN append reservations after 1 MiB of original
data. Active-session cleanup and local-floor-only cleanup must refuse; a
completed global revocation barrier resolves 32 reservations, then all daemons
crash. Offline inspection proves the remaining 32 still OPEN. Node 1 becomes an
inode-group-only leader; management redirects cleanup to a dual-host coordinator
which resolves the remaining 32; exact-epoch replay proves the phase empty.
Original 1 MiB plus 8 MiB of zero holes verified before/after cold restart,
then physical reclaim, inventory/quota/late-PUT GC gates. Method and scenario
are in README.md and session-append.json; commands via the `raft-session`
management binary.

## Results

| Gate | Result |
|---|---|
| Active/local-floor-only cleanup refusal | PASS (rc=-13 before barrier) |
| Global barrier + first 32 resolutions | PASS |
| Crash + offline proof of 32 OPEN + redirect + remaining 32 | PASS |
| Cold byte verification (1 MiB + 8 MiB holes) | PASS |
| Core POSIX | 212/212 PASS |
| Peer POSIX | 64/64 PASS |
| Full Linux regressions and focused unit tests | PASS |
| Physical reclaim / inventory / quota / late-PUT fences | PASS |

## Limitations and remaining gates

- Only the old-session OPEN reservation phase is finished; DONE records/append
  receipts and PUT tickets remain separate retirement dependencies.
- Legacy `fence` unchanged; D25 and production session activation remain staged.
- Selection has fixed memory, but streaming discovery/frontier scans have no
  whole-shard CPU/time bound.
- Single-host four-daemon crash gate, not hardware power-loss proof.

## Evidence index

| File | sha256 (16) |
|---|---|
| README.md | c602e2e3de7677a6 |
| session-append.json | d4a78a90fce07787 |
| live.log | efe6f7eda06c6ec3 |
| unit.log | 839b2846a32b5c7d |
| linux-tests.log | a8eae4d2fb81f4e2 |
