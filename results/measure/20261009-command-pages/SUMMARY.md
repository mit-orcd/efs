# SUMMARY — results/measure/20261009-command-pages

Command pagination gates: paged find/stats across refresh, permission and
peer-mutation boundaries, with full suites.

## Purpose and linked item

- `.efs` command paging (query pagination). Fixture with deployment records.

## Verdict

**PASS**: page-gates.log — "100 default, size/limit bounds, complete NUL and
NDJSON export PASS; pinned generation survives refresh; no missing/duplicate
entries; close/stale PASS; fresh permission checks across pages PASS; source
reads, command listing, lookup-only traversal and help PASS; peer catalog, raw
filenames, JSON, NUL, terminal escaping and immediate listing PASS; truthful
stats and immutable open/seek/fstat across peer mutation PASS". posix.tsv has
0 FAIL rows; peer.tsv retained; unit runs query-pages-*.log.

## Source commit and build identity

Per `deployment/` records in this dir.

## Environment

NUC deployment fixture; peer two-mount layout (peer.tsv).

## Method

Page/virtual gate logs + full posix/peer TSVs + unit runs
(query-pages-all-units.log/.status, query-pages-final*.log).

## Results

All page gates PASS; posix 0 FAIL.

## Limitations and remaining gates

Expiry unavailable (recorded in the gate lines where applicable).

## Evidence index

- `page-gates.log`, `virtual-gates.log`, `query-pages-final.log`,
  `query-pages-final-python.log`, `query-pages-final-unit.log`,
  `query-pages-all-units.log`, `query-pages-all-units.status`, `posix.tsv`,
  `peer.tsv`, `deployment/` (run `shasum -a 256 <file> | cut -c1-16` here)
