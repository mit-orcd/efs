# SUMMARY — results/measure/20261009-durability-tracepoints

NUC durability batching rerun using kernel tracepoints: five-minute recording
without strace, confirming expensive NVMe persistence with modest typical WAL
batching.

## Purpose and linked item

- Durability batching measurement method change: perf/tracepoint attach instead
  of strace (README.md).

## Verdict

**PASS**: fresh SSH access can read syscall tracepoints and attach perf to the
existing daemons — no cluster restart, deployment, production-code or
durability-setting change was required.

## Source commit and build identity

Not recorded in the run (measurement of the installed NUC daemons).

## Environment

NUC production daemons, NVMe storage; 5-minute tracepoint recording, no
strace.

## Method

perf + syscall tracepoints attached to running daemons; no strace.

## Results

Confirms expensive NVMe persistence with modest typical WAL batching
(README.md result section).

## Limitations and remaining gates

Five-minute window; method demonstration plus the batching finding.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  trace captures in this dir
