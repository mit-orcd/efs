# SUMMARY — results/measure/20261009-w92-clean-cluster

W92 clean xorinox acceptance: user-authorized full wipe of all three dedicated
storage volumes; historical wrong-placement metadata erased, not repaired.

## Purpose and linked item

- **W92** clean-cluster acceptance on xorinox.

## Verdict

**PASS** (per README.md): production binaries `47e7b886` (devel-find-stats,
files/direct-I/O/TCP, runtime user efs); ordinary mounts opt out of commands;
the dedicated acceptance mount opted in and was drained/removed afterward;
normal services and mounts restored. Historical wrong-placement metadata was
erased, not repaired; its provenance remains unknown.

## Source commit and build identity

Production binaries `47e7b886`, devel-find-stats (README.md).

## Environment

xorinox three storage VMs (wiped with explicit user authorization), TCP,
direct I/O, files engine; acceptance mount opt-in, drained/removed after.

## Method

Wipe → fresh acceptance run → drain/remove acceptance mount → restore normal
services (README.md).

## Results

Clean acceptance passes per README.md.

## Limitations and remaining gates

The erased metadata's provenance is unknown (recorded, not repaired).

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
