# SUMMARY — results/measure/20261009-transaction-durability

Transaction persistence must remain retryable: two reproduced failures and
their fixes.

## Purpose and linked item

- Transaction durability: (1) host apply converted PREPARE/DECIDE/RESOLVE/DROP
  IO/NOMEM into an advanced Raft apply index; (2) RESOLVE replaced a failed
  version read with zero and committed anyway.

## Verdict

**FAIL→fixed** (regression pair): the actual extracted return-policy regression
failed before the fix and passes after; the AMD fault regression returned OK
for an injected IO error before the fix. Both failure modes reproduced and
retained in the logs.

## Source commit and build identity

Per README.md.

## Environment

Extracted return-policy regression + AMD fault injection.

## Method

Before/after regression pairs per README.md.

## Results

Both defects fixed; pre-fix failure evidence retained.

## Limitations and remaining gates

Per README.md scope.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  regression logs in this dir
