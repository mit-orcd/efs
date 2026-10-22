# SUMMARY — results/measure/20261009-mac-devel

Mac devel integration, FIRST snapshot (superseded): unit suite and native
baseline pass; the EFS posix run failed broadly (137/212) and the snapshot was
replaced by `20261009-mac-devel-final`.

## Purpose and linked item

- Mac devel integration snapshot 1 of 2. Source `0abe7fd74078` on `devel`.

## Verdict

**FAIL** (superseded, retained for history): full `make test` passed
(deploy-unit.log); native POSIX baseline 212/212; EFS POSIX jobs=4 timeout=30 s:
**137/212 passed, 14 failed, 61 not run** (posix.tsv/posix.log retain details;
large-directory case uses its own 75 s budget). Two-client run partial —
terminated deliberately when the final merged snapshot passed its unit suite;
not a completed gate; persistence stages not run. Deployment initially failed
creating the gateway tests parent (deploy-finish.log records the corrected
harness transfer).

## Source commit and build identity

`0abe7fd74078` on devel (README.md).

## Environment

Fedora/aarch64 Tart storage VMs efs1–efs3 + gateway efsgw; TCP, buffered
storage, existing data preserved; everything runs as Linux user `efs`.

## Method

Deploy + unit suite + native/EFS posix + partial two-client per README.md.

## Results

See verdict; this snapshot is superseded by the final integration run.

## Limitations and remaining gates

Superseded — do not cite as the Mac integration verdict; use
`20261009-mac-devel-final`.

## Evidence index

- `README.md`, `deploy-unit.log`, `posix.tsv`, `posix.log`,
  `deploy-finish.log` (run `shasum -a 256 <file> | cut -c1-16` here)
