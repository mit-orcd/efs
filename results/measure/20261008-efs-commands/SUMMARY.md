# SUMMARY — results/measure/20261008-efs-commands

EFS v2 command interface gates: JSON stats/find/ls, immutable catalog identity,
explicit limits, two-mount virtual interface, full posix + peer suites,
restart rebuild, publication smoke.

## Purpose and linked item

- `.efs` v2 command interface (query_filters.h included here). Build:
  `c22754236b38-cmd-3997575c` on branch devel-find-stats (built
  2026-10-08T16:04:43Z, from command-final-build.log).

## Verdict

**PASS** (all gates): "v2 command JSON stats/find/ls, immutable catalog
identity and explicit limits PASS; two-mount virtual interface gates PASS;
posix full suite PASS; peer full suite PASS; restart rebuild from persisted
metadata PASS; query_publication: PASS (smoke only)".

## Source commit and build identity

`c22754236b38-cmd-3997575c`, branch devel-find-stats, built
2026-10-08T16:04:43Z; capacity build variant in command-capacity-build.log.

## Environment

Fixture dirs `clean-fixture/`, `capacity-clean-fixture/`; builds logged in
command-*-build.log.

## Method

Gate logs: `command-clean-gates.log`, `command-capacity-gates.log`,
`command-capacity-unit.log`, `command-final-unit.log`.

## Results

All command-interface gates PASS (see verdict list).

## Limitations and remaining gates

query_publication gate is smoke-only.

## Evidence index

- `build-id.txt`, `command-clean-gates.log`, `command-capacity-gates.log`,
  `command-final-build.log`, `command-final-unit.log`,
  `command-capacity-build.log`, `command-capacity-unit.log`, `query_filters.h`
  (run `shasum -a 256 <file> | cut -c1-16` here)
