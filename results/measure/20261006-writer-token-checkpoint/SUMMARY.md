# Writer token integration checkpoint — Oct 6 2026

## Purpose and linked item

Staged D25 ownership checks before the admission-routing decision recorded
in `docs/status/d25-admission-routing.md`. Three implementation rounds:

1. `dccd8a57`: publication surviving ranges must be subsets of original
   accepted ranges with the same admission epochs. Invalid flags,
   manufactured holes and future ages fail before token creation; outputs
   remain unchanged.
2. `a5dfacd8`: validate tokens again on completion. Malformed tokens,
   invalid ownership and mismatched FileID cannot clear pending publication
   state or advance authority. A legitimate concurrent rewrite still
   retains its bytes while retiring the matching older committed
   publication.
3. Cache rebinding preserves chunk geometry; publication binding rejects
   mismatched geometry and snapshot mutations newer than accepted
   ownership.

Added by commit `7b46f911f fix(d25): enforce cache and publication geometry
before binding`.

## Verdict

PASS — full isolated Linux `make all test` PASS after each round; local
range and allocator/publication regressions pass normally and under
ASan/UBSan; cache-binding production-function regression PASS; documentation
gate 5/5 PASS.

## Source commit and build identity

Working-tree snapshot: compile lines in `linux-tests.log` show build id /
version / branch all `unknown`, build time 2026-10-06T19:10:26Z, gcc
`-O3 -march=native`. Round commits `dccd8a57` and `a5dfacd8` named above;
adding commit `7b46f911f`.

## Environment

Isolated Linux build/test host (NUC source snapshot per the D25 series; not
explicitly stated in this run). No live cluster: no live workload or
deployment was needed for these staged ownership checks. Original mounts
and retained-write evidence unchanged.

## Method

`make all test` after each of the three rounds, plus targeted regression
suites (range, allocator/publication, cache-binding production functions)
under normal and ASan/UBSan builds, and the 5-item documentation gate. Raw
output: `linux-tests.log` (all suites OK; per-suite PASS lines for fence
reads, writeback recovery, STALE recovery, lookup memo, fold observation,
writer fence merge, writer view RPC, create errors, and the unit-test
binaries).

## Results

| check | result |
|---|---|
| Linux `make all test` (each round) | PASS (20 OK/PASS lines, no failures in log) |
| ASan/UBSan range + allocator/publication | PASS |
| Cache-binding production-function regression | PASS |
| Documentation gate | 5/5 PASS |

## Limitations and remaining gates

- Epoch-aware FUSE admission remains inactive; staged ownership checks
  only — no live gate claimed.
- The routing choice itself is recorded in
  `docs/status/d25-admission-routing.md`.

## Evidence index

- `linux-tests.log` (eb2b0dd6f3bf9a4e) — full build + test output, all suites OK
