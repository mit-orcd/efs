# SUMMARY — results/measure/20261009-catalog-ordinal

Catalog ordinal run: live find output (NDJSON) over the tiny-command-freshness
tree with before/after annotation, children, publications and record captures.

## Purpose and linked item

- Catalog ordinal/pagination evidence over a live tiny-file tree (find with
  snapshot, page token/cursor, more/more_confirmed fields).

## Verdict

**PASS** (capture completed): `live-find.json` is a complete find response
(version 1, complete=true, snapshot boot/generation/applied state,
refresh_error=0, page token+cursor with more=true) over the
tiny-command-freshness tree; before/after series (`after-annotate.txt`,
`after-children.txt`, `after-latest.txt`, `after-publications(-warm).txt`,
`after-record.log`, `after-screen.txt`, `after-self.txt`, `after.data`,
`build-annotate.txt`, `children.txt`) plus `deployment/`.

## Source commit and build identity

Per `deployment/` records in this dir.

## Environment

Live NUC deployment over the tiny-command-freshness workload tree.

## Method

Live find with snapshot/pagination + before/after annotation captures.

## Results

Complete find with valid pagination state (see live-find.json).

## Limitations and remaining gates

Capture over one tree state; the catalog ordinal gates proper are the unit
runs of the W92 line.

## Evidence index

- `live-find.json` — complete find response (run `shasum -a 256
  live-find.json | cut -c1-16` here)
- `after-*.txt`, `children.txt`, `after.data`, `deployment/`
