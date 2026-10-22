# SUMMARY — results/measure/20261009-mac-devel-repair

Mac devel integration and failure repair: initial integration validated at
`9c4db072de1a`; final frozen branch-tip batch merged at `553fbb42d738`.

## Purpose and linked item

- Mac devel integration repair line: `integration-refs-final.tsv` and
  `integration-ancestry-final.tsv` define and verify the complete final inputs.

## Verdict

**PASS** (per README.md): initial integration validated at `9c4db072de1a`;
the final batch at `553fbb42d738` with ancestry verified. Failures found during
the campaign are retained in the logs (see README.md) and repaired in the final
merge.

## Source commit and build identity

Initial `9c4db072de1a`; final `553fbb42d738` (README.md).

## Environment

Three Fedora/Tart storage VMs (TCP, buffered XFS data volumes, 2+1 coding) +
separate gateway VM; Linux servers/clients/builds/tests as `efs`.

## Method

Integration validation with reference/ancestry TSVs; repair loop documented in
README.md.

## Results

Final merged batch verified per README.md.

## Limitations and remaining gates

Buffered XFS; see README.md for the retained intermediate failures.

## Evidence index

- `README.md`, `integration-refs-final.tsv`, `integration-ancestry-final.tsv`
  (run `shasum -a 256 <file> | cut -c1-16` here)
