# SUMMARY — results/measure/20261008-xorinox-known-bugs

W58/W55 evidence on isolated xorinox fixtures (branch devel-known-bugs from
`3e1d4c6e`), installed three-VM services preserved.

## Purpose and linked item

- **W58/W55** known-bug qualification: lost-reply identity, PUT-loss/collision,
  enclosing-GC, cold acceptance, and fragment-fsync refusal/recovery.

## Verdict

**PASS** (per README.md): `combined/` (manifest, lost-reply identity record,
PUT-loss/collision record, POSIX result tables, compact enclosing-GC evidence);
`unique/` (1,024-file/16-worker cold acceptance); `barrier/` (server
fragment-fsync refusal/recovery); complete harness phase results in
`live.log`, `unique-final.log`, `barrier.log`; final focused regressions in
`final-unit.log`.

## Source commit and build identity

Branch devel-known-bugs from `3e1d4c6e`.

## Environment

Isolated four-node files/direct/TCP fixtures inside xefs1; installed xorinox
three-VM services preserved, not redeployed.

## Method

Per-fixture harness runs with manifests; see README.md's per-fixture links.

## Results

All fixture gates pass per README.md.

## Limitations and remaining gates

Fixture scope; production xorinox gates are separate runs.

## Evidence index

- `README.md` — narrative + links (run `shasum -a 256 README.md | cut -c1-16`
  here); `combined/`, `unique/`, `barrier/`, `live.log`, `unique-final.log`,
  `barrier.log`, `final-unit.log`
