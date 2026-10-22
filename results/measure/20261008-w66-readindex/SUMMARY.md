# SUMMARY — results/measure/20261008-w66-readindex

W66 outstanding-batch ReadIndex regression, deterministic protocol evidence on
an in-memory three-voter Raft network.

## Purpose and linked item

- **W66**: the new test delivers a durable leader's data batch to followers but
  withholds its replies, then releases delivery and begins a fresh read round
  without advancing timer ticks. Baseline suppresses outgoing messages because
  commit has not advanced and the prior batch is fresh — the new read stays
  pending.

## Verdict

**FAIL** (regression demonstrated on baseline): the fresh read stays pending on
baseline production code `532660e3` (same runtime code as `3f8c9f81`).

## Source commit and build identity

Baseline `532660e3` (README.md).

## Environment

Local Mac run, isolated in-memory three-voter Raft network — deterministic
protocol evidence, not AMD latency or Linux acceptance.

## Method

In-memory Raft fixture; withheld-reply batch then fresh read round (README.md).

## Results

Baseline: new read stays pending (regression). Fix evidence is the W66 series
(e.g. `20261008-w66-mac`, `20261010-w66-*`).

## Limitations and remaining gates

Deterministic protocol scope only.

## Evidence index

- `README.md` — scenario + baseline behavior (run `shasum -a 256 README.md |
  cut -c1-16` here); test logs in this dir
