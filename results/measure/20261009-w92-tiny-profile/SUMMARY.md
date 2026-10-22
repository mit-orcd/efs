# SUMMARY — results/measure/20261009-w92-tiny-profile

W92 tiny-file/cache profiling on xorinox with final code `6e841662`
(devel-find-stats).

## Purpose and linked item

- **W92** tiny-file and cache profiling; both generated trees remain in EFS;
  the diagnostic storage-node mount was removed after cold verification;
  normal gateway/client mounts remain opted out.

## Verdict

**PASS** (per README.md): workload and daemon profiles completed on the final
deployed binaries (details in README.md's workload/daemon profile sections).

## Source commit and build identity

`6e841662` on devel-find-stats (README.md).

## Environment

Xorinox, files engine, direct fragment I/O, TCP, runtime user efs on all three
VMs; no wipe.

## Method

Tiny-file workload + daemon profiles per README.md.

## Results

Profiles completed and retained.

## Limitations and remaining gates

Per README.md.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  captures in this dir
