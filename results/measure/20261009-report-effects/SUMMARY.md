# SUMMARY — results/measure/20261009-report-effects

REPORT activation and completion session guards: lane activation appends the
full mount UUID/epoch and checks the inode floor at serialized apply.

## Purpose and linked item

- REPORT session guards: both single-lane fallback and cached mask activation
  retain old-log decoding; REPORT append completion PREPAREs the reporting
  session's exact local row before inode/cursor/lane images in the same
  transaction/participant set.

## Verdict

**PASS** (per README.md): the listed guard behaviors verified on the AMD
private candidate.

## Source commit and build identity

Per README.md (AMD private candidate).

## Environment

AMD private fixture, direct I/O/TCP.

## Method

Activation/completion guard tests per README.md.

## Results

All listed behaviors verified.

## Limitations and remaining gates

Candidate scope; production rollout is a later step.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
