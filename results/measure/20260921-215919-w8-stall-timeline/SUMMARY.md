# SUMMARY — results/measure/20260921-215919-w8-stall-timeline

W8 timeline capture: 9-host posix suite TSVs plus raft term/leader transitions,
correlating suite slowdowns with raft elections.

## Purpose and linked item

- **W8** (posix storm). Timeline evidence pairing suite results
  (`results/posix/20260922-015936`, suite start epoch 1790042376) with
  `ops.tsv` and `raft.tsv` (term/leader changes). Companion of the W8 studies
  `20260921-202253-w8-root-srv` / `20260921-194332-w8-root`.

## Verdict

**PASS** (capture completed): suite 191–195/201 per host (fail 5–9, skip 1
mmap, 90–103 s each) while the raft log shows a g0 leader transition
(FOLLOWER→LEADER, term 5272→5273) mid-suite — the timeline correlation the W8
stall analysis needed.

## Source commit and build identity

Not recorded in the run.

## Environment

9 hosts fcstor007–015 running the posix suite concurrently; raft groups g0/g2
observed on fcstor003–006.

## Method

Concurrent 9-host posix suite with observers: per-host `obs-fcstor*.txt`,
operation timeline `ops.tsv`, raft transitions `raft.tsv`, suite TSVs under
`results/posix/20260922-015936` (referenced).

## Results

| host | pass/201 | fail | dur s |
|---|---|---|---|
| fcstor007 | 191 | 9 | 90.1 |
| fcstor008–015 | 191–195 | 5–9 | 97.5–103.0 |

Raft: g0 FOLLOWER→LEADER at term 5273 (commit 5136441→5165163) during the
suite; g2 stable FOLLOWER.

## Limitations and remaining gates

Correlational evidence; the causal stall analysis is in the W8 root-srv strace
run. Failures listed in the TSVs are the era's known W8 residuals.

## Evidence index

- `REPORT.txt` — suite summary + raft timeline (run `shasum -a 256 REPORT.txt |
  cut -c1-16` in this dir)
- `ops.tsv`, `raft.tsv` — timelines
- `obs-fcstor003..006.txt`, `preflight.txt`
