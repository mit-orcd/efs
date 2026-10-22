# SUMMARY — results/measure/20261009-hashed-pending-create

First-use cross-group pending CREATE: the full UUID/epoch/token lease rides in
the first-use HASHED lane transaction.

## Purpose and linked item

- Pending-CREATE cross-group correctness. Baseline `dd3e4c9a`.

## Verdict

**PASS** (per README.md): AMD direct-I/O/TCP private fixture
`gc-direct-vx03yp12` passes local and first-use cross-group HASHED CREATE,
same-inode retry, cold restart, and exact pending adoption. An exclusive,
exact-value local session participant protects admission against concurrent
local fencing until the transaction decision resolves. Existing legacy HOLD
encoding is preserved.

## Source commit and build identity

Baseline `dd3e4c9a` (README.md).

## Environment

AMD private fixture gc-direct-vx03yp12, direct I/O/TCP.

## Method

First-use cross-group HASHED CREATE + retry + cold restart + adoption oracles.

## Results

All listed scenarios pass per README.md.

## Limitations and remaining gates

Legacy HOLD encoding preserved (scope note).

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  fixture logs in this dir
