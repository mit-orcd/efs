# SUMMARY — results/measure/20261008-w66-mac

W66 Mac buffered/TCP evidence: latency matrix on a private four-daemon Mac VM
fixture, baseline vs candidate.

## Purpose and linked item

- **W66** (latency): candidate `2b9da49f`; baseline replaces only raft.c with
  the pre-fix `532660e3` version. Run order latency-a1, latency-b1-retry,
  latency-b2, latency-a2.

## Verdict

**FAIL** (partial): full make test and stale-leader freshness passed, but
**POSIX failed: 145 pass, 7 fail, 60 not run out of 212**; peer/final recovery
not reached. The initial b1 start hit TIME_WAIT; no measured samples were used
from it.

## Source commit and build identity

Candidate `2b9da49f`, baseline raft.c `532660e3` (README.md).

## Environment

Private four-daemon fixture on Mac cluster VM efs1, buffered/TCP; installed
services untouched. Each results.json contains all GETATTR samples, mutation
attempts and before/after Raft status.

## Method

Latency runs a1/b1-retry/b2/a2 with per-run results.json; fixture manifest and
logs retained.

## Results

| gate | result |
|---|---|
| make test | PASS |
| stale-leader freshness | PASS |
| POSIX | FAIL (145/212; 7 fail, 60 notrun) |
| peer/final recovery | NOTRUN |

## Limitations and remaining gates

The POSIX failure blocks W66 acceptance here; the TIME_WAIT initial b1 start is
recorded as harness noise (no samples used).

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  per-latency-run results.json + logs in this dir
