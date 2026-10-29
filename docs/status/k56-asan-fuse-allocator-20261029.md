# K56 — ASan-built efs-fuse refuses to start

**FIXED** on `k56-asan-allocator` (fix `ae655e066`), blocks alpha: no. Found by
[Task AL](al-decode-scratch-20261028.md): `make asan-check` failed five tests
at startup, identically on clean devel and on AL's change.

## Cause

Task I's allocator policy (wave 31, `fuse_allocator_init` in
`src/client/efs_fuse.c`) calls `mallopt(M_ARENA_MAX, 4)` and
`mallopt(M_MMAP_THRESHOLD, 128 KiB)` and refuses to start unless both succeed.
An ASan build replaces `malloc`, and its `mallopt` always returns failure, so
`efs-fuse` exited with `cannot establish allocator policy` before reaching
any test. The 49 ASan/UBSan/LeakSanitizer checks on intel2 at `b9c2fad85`
predate Task I, and no gate runs `make asan-check`, so nothing caught it.

## Fix

The policy is skipped when `__SANITIZE_ADDRESS__` is defined and stays fatal
in every other glibc build. No switch, no behavior selector. Limit: this is
gcc's macro (the sanitizer gate's compiler); a clang ASan build needs a
`__has_feature(address_sanitizer)` test (comment in the source).

## Evidence

Host xorinox (16 CPUs), `bash tests/sanitizer_check.sh asan`:

| Source | Result |
| --- | --- |
| devel `330c48aea` | 49 run / **5 failed**, 60 s; `env_removed`, `k36_recovery`, `k36_segment_recovery`, `k37_segment_rebuild`, `d4_progress`, each `AssertionError: efs-fuse: cannot establish allocator policy` |
| fix `ae655e066` | 49 run / **0 failed**, 425 s |

A normal (non-ASan) build of the fixed tree starts: `./efs-fuse --help` exits
0. Raw logs: xorinox `~/efs-artifacts/k56-20261029/` (`asan-baseline-330c48aea.log`,
`asan-fixed-ae655e066.log`), manifest `SHA256SUMS` sha256
`64189a30a0baaacb23ae31eb169c704db144f65ff2f23ca852354094b775192c`.

## Not covered

No full `make test-all` ran on the fixed tree: the changed line is compiled
only into ASan builds. The refusal in a normal glibc build when `mallopt`
fails is unchanged and not exercised here.
