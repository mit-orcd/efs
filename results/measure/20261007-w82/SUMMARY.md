# SUMMARY — results/measure/20261007-w82

W82 read-freshness partition test: baseline vs fixed, using
tests/live/raft_read_freshness.py.

## Purpose and linked item

- **W82** (read freshness under partition): an isolated leader must not serve
  stale authority.

## Verdict

- Baseline: **FAIL** — `assert all('status=0' not in reply ...)` traceback: a
  stale read was accepted on the unfixed build.
- Fixed: **PASS** — "majority changed root mode; eight isolated-leader GETATTRs
  refused stale authority; healed forwarded read True".

## Source commit and build identity

Not recorded in the run (baseline/fixed partition logs only).

## Environment

Test fixture from `/tmp/efs-w82-baseline` / `/tmp/efs-read-freshness-*`;
initial leader 1, majority leader 0; root setattr used as the authority probe.

## Method

`tests/live/raft_read_freshness.py` per build: `baseline-partition.txt`,
`fixed-partition.txt`.

## Results

| build | result |
|---|---|
| baseline | FAIL (stale GETATTR accepted) |
| fixed | PASS (8 isolated-leader GETATTRs refused; healed forward works) |

## Limitations and remaining gates

Single partition scenario; W82's broader matrix is tracked in the work item.

## Evidence index

- `baseline-partition.txt`, `fixed-partition.txt` (run `shasum -a 256 <file> |
  cut -c1-16` here)
