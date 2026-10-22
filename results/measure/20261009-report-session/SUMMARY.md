# SUMMARY — results/measure/20261009-report-session

REPORT mapping session boundary: full mount UUID/epoch suffix on production
REPORT with a separate Raft publication envelope and strict lane session floors
at serialized apply.

## Purpose and linked item

- REPORT session boundary (AMD direct/TCP candidate).

## Verdict

**PASS** (per README.md): existing floors checked under the batch ReadIndex;
missing/BUSY admission goes through authoritative global CREATE/REGISTER/
ESTABLISH; packed forwarding preserves variable delta records and flags;
malformed/trailing wire bytes fail.

## Source commit and build identity

Per README.md (candidate).

## Environment

AMD private fixture, direct I/O/TCP.

## Method

Session-boundary gates per README.md.

## Results

All listed behaviors verified.

## Limitations and remaining gates

Candidate scope; production rollout separate.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
