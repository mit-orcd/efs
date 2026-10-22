# SUMMARY — results/measure/20260923-025903-i17-leader-freeze

I17 leader-freeze rerun: the recovery gate after the previous day's FAIL.

## Purpose and linked item

- **I17** (raft leader freeze/resume fault). Rerun of
  `20260922-042153-i17-leader-freeze` (FAIL).

## Verdict

**PASS** (`result.txt` = PASS): frozen-leader recovery is clean on this build;
leader pids thawed (`freeze.txt`: THAWED 761104/693703), before/after state
consistent.

## Source commit and build identity

Not recorded in the run.

## Environment

fcstor cluster raft groups, leader freeze via SIGSTOP/SIGCONT; observers
before/after.

## Method

Freeze leader, observe (`obs-before/after.txt`), recover and re-observe
(`recover-before/after.txt`), verdict in `result.txt`.

## Results

PASS at 2026-09-23 02:59Z, closing the previous day's I17 failure.

## Limitations and remaining gates

Single freeze/recover cycle; I17 remains a fault-injection gate to re-run on
transport/apply changes.

## Evidence index

- `result.txt` — verdict (run `shasum -a 256 result.txt | cut -c1-16` here)
- `freeze.txt`, `obs-before.txt`, `obs-after.txt`, `recover-before.txt`,
  `recover-after.txt`
