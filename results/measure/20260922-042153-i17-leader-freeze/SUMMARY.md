# SUMMARY — results/measure/20260922-042153-i17-leader-freeze

I17 leader-freeze test: freeze a raft leader mid-operation and verify recovery.

## Purpose and linked item

- **I17** (raft leader freeze/resume fault). Companion runs:
  `results/measure/20260922-122517-i17-leader-freeze`,
  `20260923-025903-i17-leader-freeze` (the passing rerun).

## Verdict

**FAIL** (`result.txt` = FAIL). The frozen-leader run did not recover cleanly;
both efsd pids were thawed after (`freeze.txt`: THAWED 749444/454003).
Diagnosis detail lives in the freeze/obs/recover captures.

## Source commit and build identity

Not recorded in the run.

## Environment

fcstor cluster raft groups, leader freeze via SIGSTOP/SIGCONT on the leader
pid; observers before/after.

## Method

Freeze leader (`freeze.txt`), observe state (`obs-before/after.txt`), recover
and re-observe (`recover-before/after.txt`), verdict in `result.txt`.

## Results

FAIL at 2026-09-22 04:21Z; the same test passes the next day
(`20260923-025903-i17-leader-freeze`).

## Limitations and remaining gates

See the passing rerun for the closure evidence.

## Evidence index

- `result.txt` — verdict (run `shasum -a 256 result.txt | cut -c1-16` here)
- `freeze.txt`, `obs-before.txt`, `obs-after.txt`, `recover-before.txt`,
  `recover-after.txt`
