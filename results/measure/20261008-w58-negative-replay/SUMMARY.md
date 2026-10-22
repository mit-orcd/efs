# SUMMARY — results/measure/20261008-w58-negative-replay

W58 negative-replay regression: test_meta_apply negative-verdict cases fail on
the tested tree.

## Purpose and linked item

- **W58** (negative replay): a failed negative verdict must not stay ambiguous
  or leave ownership effects.

## Verdict

**FAIL** (`meta-red-green.txt`): test_meta_apply.c failures — line 5599
"failed negative verdict remains ambiguous", 5600 "negative verdict has no
ownership effect", 5604 "durable negative verdict", 5607 "late CREATE cannot
acquire abandoned ownership".

## Source commit and build identity

Not recorded in the run.

## Environment

Unit-test level (test_meta_apply), no cluster.

## Method

`tests/test_meta_apply` red/green run (`meta-red-green.txt`).

## Results

Four negative-verdict assertions fail on the tested tree — the W58 defect
record.

## Limitations and remaining gates

Unit-level evidence; the fix's green state is a later run.

## Evidence index

- `meta-red-green.txt` — failures (run `shasum -a 256 meta-red-green.txt |
  cut -c1-16` here)
