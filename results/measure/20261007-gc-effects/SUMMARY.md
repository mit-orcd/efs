# SUMMARY — results/measure/20261007-gc-effects

GC effect measurement: before/after GC pass counters for the posix workload
and a last-close case, on the direct-I/O path.

## Purpose and linked item

- GC reclamation effect evidence (chunk/fragment GC removing dead fragments and
  reclaiming payload bytes) — supports the GC-health work of early October.

## Verdict

**PASS** (GC effective): direct-posix case: 60 → 471 GC passes, removed
fragments 270 → 22 608, reclaimed payload 17.7 MB → 1.48 GB, ending data usage
0 B. Last-close case (ino 61441): 71 → 76 passes, 135 → 354 removed fragments,
8.8 MB → 23.2 MB reclaimed, data usage 30.5 MB → 16.1 MB, remaining target
fragments 0.

## Source commit and build identity

Not recorded in the run.

## Environment

Direct-I/O fixture (per filename); single target inode for the last-close case.

## Method

Workload runs (posix; one tracked last-close), GC pass counters sampled
before/after (`buffered-last-close.json`, `direct-last-close.json`,
`direct-posix.json`).

## Results

| case | passes before→after | removed fragments | reclaimed payload | final data usage |
|---|---|---|---|---|
| direct-posix | 60 → 471 | 270 → 22 608 | 17.7 MB → 1.48 GB | 0 B |
| direct-last-close (ino 61441) | 71 → 76 | 135 → 354 | 8.8 MB → 23.2 MB | 30.5 → 16.1 MB |

## Limitations and remaining gates

Counters only; physical reclamation timing is the gc-health runs' topic.

## Evidence index

- `direct-posix.json`, `direct-last-close.json`, `buffered-last-close.json`
  (run `shasum -a 256 <file> | cut -c1-16` here)
