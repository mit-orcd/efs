# SUMMARY — results/measure/20261008-publication-pending

Publication pending evidence on a private AMD source, hash-pinned.

## Purpose and linked item

- Publication-pending behavior; scope and results in
  `docs/status/publication-pending-20261008.md`. Baseline `6336e0d0` plus the
  source SHA-256 manifest (`source-sha256.txt`).

## Verdict

**PASS** (per README.md): full units passed before the final two test-only
extensions; those targeted tests then passed (`unit.log`, `targeted.log`,
`live.log`).

## Source commit and build identity

Baseline `6336e0d0` + source SHA-256 manifest; binaries report unknown version
because the transferred tree has no git metadata — `service-and-binaries.txt`
and the manifest govern identity.

## Environment

Private AMD source `/data1/efs/evidence/warnings-src`.

## Method

Units → targeted tests → live run per README.md.

## Results

All pass per README.md and the status page.

## Limitations and remaining gates

See `docs/status/publication-pending-20261008.md` for scope.

## Evidence index

- `README.md`, `source-sha256.txt`, `service-and-binaries.txt`, `unit.log`,
  `targeted.log`, `live.log` (run `shasum -a 256 <file> | cut -c1-16` here)
