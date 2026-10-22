# SUMMARY — results/measure/20261008-v020-amd

AMD v0.2.0-pre-alpha acceptance across modes: full POSIX, posix2, durability
prepare/verify per mode, with hash-pinned deployed source.

## Purpose and linked item

- v0.2.0-pre-alpha acceptance: merged source `2ba0d7cc` (merge `952b57d5`).

## Verdict

**PASS** (per README.md): full clean Linux build + `make test`; deployed source
hashes match 349 tracked build/test files exactly; each mode ran full POSIX,
two-client POSIX2, then 26 durability prepare and 26 cold-remount verify cases.
The runner verified all four daemon I/O flags, both client transport settings
and uverbs presence for all six RDMA processes. mmap remains the one known
unsupported POSIX skip. Matrix and TSVs contain exact verdicts.

## Source commit and build identity

`2ba0d7cc` (merge `952b57d5`); build metadata supplied explicitly (copied
worktree .git cannot resolve on AMD); all processes use the same candidate
binary hashes.

## Environment

AMD host, four daemons, modes direct/buffered × TCP/RDMA; strict RDMA uses
active RXE rxe0/GID1 (software RoCE — not hardware offload or native-IB
acceptance).

## Method

Per-mode suite matrix with verified flags; durability driver no longer forces
TCP on remount (fix in devops repo `e0fcc1f`); remote scripts invoked with
explicit settings. No force-discard/reformat/test-only fs behavior.

## Results

All modes pass per README.md; exact verdicts in the matrix TSVs.

## Limitations and remaining gates

Software RoCE on AMD; native-IB acceptance is the orcd-* runs.

## Evidence index

- `README.md` — acceptance narrative (run `shasum -a 256 README.md |
  cut -c1-16` here); matrix TSVs and per-mode logs in this dir
