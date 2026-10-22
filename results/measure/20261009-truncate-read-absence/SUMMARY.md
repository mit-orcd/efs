# SUMMARY — results/measure/20261009-truncate-read-absence

Authoritative absence after peer truncate: published-image retry consumes the
successful GETCHUNKS absence bitmap and returns an observed hole.

## Purpose and linked item

- Read-after-truncate correctness: the retained staging map is not proof that a
  published chunk still exists.

## Verdict

**PASS** (per README.md): published-image retry consumes the GETCHUNKS absence
bitmap, returns an observed hole and clears its publication observation without
removing locally owned staging data; failed authority reads still fail. The
deterministic regression fails on the original code and passes here.

## Source commit and build identity

Per README.md.

## Environment

Fixture per README.md (regression harness).

## Method

Deterministic regression (red on original code, green here) per README.md.

## Results

Regression green on the fixed code.

## Limitations and remaining gates

Regression scope; suite coverage in the posix trunc_* tests.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
