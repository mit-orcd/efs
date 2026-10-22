# SUMMARY — results/measure/20261009-nuc-w73-w64

NUC W73/W64 checkpoint evidence: production commit
`3e1d4c6e9c7d596c1389c8e639d423bed73ced6b` — a checkpoint, NOT green
full-matrix acceptance.

## Purpose and linked item

- **W73/W64** checkpoint on NUC.

## Verdict

**FAIL** (checkpoint with failures, per README.md): `initial-*` = complete
first direct/buffered TCP manifests and POSIX failures; `mount-sync-*` =
complete direct/buffered mount-sync verdicts (both pass);
`clean-direct/`, `clean-buffered/` = completed clean baseline POSIX manifests
and raw runner logs, but **both peer suites timed out after append EIO/BUSY**
(exact evidence in those dirs).

## Source commit and build identity

Production commit `3e1d4c6e9c7d596c1389c8e639d423bed73ced6b` (README.md).

## Environment

NUC production commit, direct and buffered TCP fixtures.

## Method

Initial manifests → mount-sync verdicts → clean baseline dirs (README.md).

## Results

| gate | direct | buffered |
|---|---|---|
| mount sync | PASS | PASS |
| clean POSIX | manifests complete | manifests complete |
| clean peer suites | TIMEOUT (append EIO/BUSY) | TIMEOUT (append EIO/BUSY) |

## Limitations and remaining gates

Explicitly not full-matrix acceptance; closure is the AMD RXE run
(`20261009-amd-w73-w64-rxe`) and its status page.

## Evidence index

- `README.md`, `initial-*`, `mount-sync-*`, `clean-direct/`,
  `clean-buffered/` (run `shasum -a 256 <file> | cut -c1-16` here)
