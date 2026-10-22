# SUMMARY — results/measure/20261009-mac-devel-final

Mac devel integration, final: `43fcefe190dd` on devel — all 15 branch tips are
ancestors; combines metadata-scaling/W98 with W66 session fences, W92 disk
quota and clean-cluster query validation, and bounded-inbox regression
coverage.

## Purpose and linked item

- Mac devel integration, final snapshot (supersedes `20261009-mac-devel`).

## Verdict

**PASS** (per README.md): the complete build and unit suite ran in
/var/tmp/efs-devel-43fcefe190dd on efs1 before installation; runtime hashes
matched on all four VMs (install.log, installed-*.txt); both efsd and efs-fuse
identify the tested devel source. Generated architecture documents regenerated
after merging; both competing unit-test additions preserved in the Makefile.
The earlier snapshot's interrupted live run is retained in
`../20261009-mac-devel/` and superseded.

## Source commit and build identity

`43fcefe190dd` (full SHA in source-commit.txt); branch tips in
branch-tips.txt; coverage check in branch-coverage.txt.

## Environment

Mac-hosted Tart Fedora/aarch64 VMs: storage efs1–efs3, gateway efsgw; TCP,
buffered storage, existing /data1 files preserved; native baselines XFS
(/dev/vda3) on the gateway; all EFS workloads as user `efs`; pressure not
enabled; no forced-discard/wipe/reset.

## Method

Build+units on efs1 → install with runtime hash match on all four VMs →
integration gates per README.md.

## Results

Integration accepted per README.md (details and gates listed there).

## Limitations and remaining gates

Buffered XFS volumes, 2+1 coding; pressure activation not enabled for these
gates.

## Evidence index

- `README.md`, `source-commit.txt`, `branch-tips.txt`, `branch-coverage.txt`,
  `install.log`, `installed-*.txt` (run `shasum -a 256 <file> | cut -c1-16`
  here)
