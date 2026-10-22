# SUMMARY — results/measure/20261008-build-warnings

Clean-build warning fixes: the 20 warnings (1 allocation-size comparison, 19
unchecked read/write/system returns) fixed and verified with -Werror.

## Purpose and linked item

- Build hygiene: warning-free build gate. Baseline `c2275423` plus the six
  source changes recorded in SUMMARY.md.

## Verdict

**PASS**: `make clean && make -j12 EXTRA_DEFS=-Werror test` exits 0, zero
`warning:` diagnostics, full unit suite passes; default optimization/fortify/
warning flags retained. Live smoke: `EFS_QUERY_RPC=1 python3
tests/live/query_publication.py /data1/efs/warnings-query-20261008
--mode direct` exits 0 — four private TCP daemons; publication, RPC
consumers/errors/redirects and persisted restart pass.

## Source commit and build identity

Baseline `c2275423` + the six fixes (per SUMMARY.md), private
`/data1/efs/warnings-src`.

## Environment

AMD host, private tree and four private TCP daemons; installed
service/stores/mounts untouched.

## Method

-Werror clean build + full unit suite; query_publication live smoke in direct
mode.

## Results

Zero warnings; unit suite green; live publication smoke green.

## Limitations and remaining gates

Not a POSIX-suite or hardware-durability gate (explicit in SUMMARY.md).

## Evidence index

- `SUMMARY.md` — full record (run `shasum -a 256 SUMMARY.md | cut -c1-16` here)
