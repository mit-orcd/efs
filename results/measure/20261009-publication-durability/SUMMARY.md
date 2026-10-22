# SUMMARY — results/measure/20261009-publication-durability

Publication and applied-index persistence boundaries: production repairs to
IO/NOMEM handling and batch error ordering.

## Purpose and linked item

- Publication durability: legacy/typed publication/admission/retirement and
  activation IO/NOMEM retain the pending Raft apply index; business conflicts
  remain ring verdicts; legacy publication batches honor hold/release errors
  and never hide a later storage failure behind an earlier STALE result.

## Verdict

**PASS** (per README.md): the listed production repairs verified; test/gate
logs in this dir.

## Source commit and build identity

Per README.md.

## Environment

Per README.md (production repair line).

## Method

Targeted persistence/ordering tests per README.md.

## Results

Repairs verified (see README.md's test list).

## Limitations and remaining gates

Per README.md scope.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  test logs in this dir
