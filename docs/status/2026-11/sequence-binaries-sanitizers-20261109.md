# The seven sequence test binaries on the sanitizer lists (Task EM)

Task EM, Nov 9 2026, node9900, devel `33cd0a839` (wave 65). Purpose: Task EE
([review of the Nov 9 feedback](review-feedback-nov9-20261109.md)) noted that
the seven sequence binaries that Task EG gave a case filter
([case filter](test-case-filter-20261109.md)) were not on the ASan or TSan
lists of `tests/sanitizer_check.sh` (`make asan-check`, `make tsan-check`),
while the smaller interface tests were.

**Verdict: PASS, test-list change only, no sanitizer finding, no K row,
blocks alpha: no.** All seven binaries are on the ASan (with UBSan and LSan)
list; the two that start threads (`test_meta_apply`, `test_session`) are
also on the TSan list beside `test_kv_lsm` and `test_raft_store`. Every new
entry passes in both lanes on node9900. The first TSan run after the change
failed one existing entry, `k37_segment_rebuild`, in a step unrelated to the
sequence binaries and without a ThreadSanitizer report; solo it passed 3 of 3
and the second full TSan run passed (see Findings). The brief's premise was
partly stale: `kv_lsm` and `raft` were already on the ASan list and `kv_lsm`
and `raft_store` on the TSan list, so this adds five ASan entries and two
TSan entries.

## The change

`tests/sanitizer_check.sh` only, one line per entry in the existing
`name|command` form, no options, no timeout change:

- ASan list, after `kv_lsm` and `raft`: `raft_store`, `meta_apply`, `sim`,
  `txn`, `session`.
- TSan list, after `raft_store`: `meta_apply`, `session`.

`test_raft`, `test_sim` and `test_txn` are on the ASan list only: they start
no thread. The check was not a source grep (only `test_kv_lsm.c` and
`test_raft_store.c` call `pthread_create` themselves; every binary links it
through `libefs.a`), but a run: each plain-built binary under
`strace -f -e trace=clone,clone3`, counting clones with `CLONE_THREAD`:

| Binary | Threads started (plain build) | Source of the threads | Lanes |
|---|---:|---|---|
| `test_meta_apply` | 1 | `efs_kv_lsm_open` → `kv_compactor_start` (`test_reap_cleanup_deferred`, found with gdb) | ASan, TSan |
| `test_raft` | 0 | none | ASan |
| `test_sim` | 0 | none | ASan |
| `test_kv_lsm` | 119 | test threads and KV compactors | ASan, TSan (both already) |
| `test_session` | 47 | stores opened by the session cases, compactors | ASan, TSan |
| `test_txn` | 0 | none | ASan |
| `test_raft_store` | 179 | test thread and store threads | ASan, TSan (TSan already) |

Fork children (`test_session`, `test_kv_lsm`, `test_raft_store`) are not
counted as threads; the TSan lane already runs with `die_after_fork=0` for
them.

## Environment

- Host node9900 (ORCD), AMD EPYC 9474F, 192 CPUs, RHEL 9 kernel
  5.14.0-687.39.1.el9_8, gcc 11.5.0, no valgrind. Other tasks ran on the
  host at the same time; the load average at each start and end is in the
  raw files (0.2 to 7.2 over the runs).
- Source: `git archive` of devel `33cd0a839` (tar sha256
  `9a1985a81b902c82f60ea82b9837e5dbf32f2b48a4362b245078c9addc55f735`, same
  on both ends) under `/tmp/efs-taskEM/src`; for the after runs the edited
  `tests/sanitizer_check.sh` (sha256 `d8b785a1015f8ab93d2b39e454d66de0cde5dce10e14d86dc9a0764fdbbef83a`)
  copied over it. `TMPDIR=/tmp/efs-taskEM-tmp`,
  `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse` with its `PKG_CONFIG_PATH` and
  `LD_LIBRARY_PATH`.
- Each lane built its own sanitized tree as the script does (ASan:
  `-fsanitize=address,undefined -fno-omit-frame-pointer -march=x86-64-v3`,
  LSan on; TSan: `-fsanitize=thread`), default per-entry timeout. The
  daemon entries take free ephemeral ports (`k33_daemons.free_port`), not
  the 17xxx live ports.

## Method

Before and after, both lanes concurrently, each in its own screen:

```
EFS_SAN_LOGDIR=/tmp/efs-taskEM/<tag>-<mode>-logs bash tests/sanitizer_check.sh <mode>
```

with `<mode>` `asan` and `tsan`, `<tag>` `before` (devel tree) and `after`
(edited script). The second TSan after run reused the after build:
`EFS_SAN_BUILD=/tmp/efs-taskEM/after-tsan-logs/build`. The k37 solo reruns:
`EFS_SAN_BUILD=… EFS_SAN_ONLY=k37_segment_rebuild bash tests/sanitizer_check.sh tsan`,
three times. Thread check: `make test-bins` on a plain copy, then per binary
`strace -f -qq -e trace=clone,clone3 tests/test_<name>` and a count of
`CLONE_THREAD`. Seconds are the gate's own per-entry figures (whole
seconds).

## Results

Seconds per binary per lane (after runs; `—` not on that lane):

| Binary | ASan s | TSan s (after) | TSan s (after2) | Status |
|---|---:|---:|---:|---|
| `test_meta_apply` | 5 | 19 | 19 | new on both |
| `test_raft` | 1 | — | — | ASan already listed (before: 1) |
| `test_sim` | 1 | — | — | new, ASan only |
| `test_kv_lsm` | 2 | 5 | 6 | both already listed (before: 3 / 6) |
| `test_session` | 0 | 21 | 20 | new on both |
| `test_txn` | 0 | — | — | new, ASan only |
| `test_raft_store` | 2 | 37 | 37 | new on ASan; TSan already listed (before: 37) |

Lane totals (the gate's `VERDICT` line, entries only; the build is extra:
ASan 59-60 s, TSan 13-14 s):

| Lane | Before (entries, s) | After (entries, s) | Verdict after |
|---|---|---|---|
| ASan | 54, 358 | 59, 369 | PASS |
| TSan | 29, 570 | 31, 705 | FAIL (`k37_segment_rebuild`, test rc 1, 154 s) |
| TSan, second run | | 31, 610 (alone on the host, no ASan lane beside it) | PASS |

The five new ASan entries add 9 s; the two new TSan entries add 40 s. Both
are small against their lane (ASan 358 s, TSan 570 s), so no binary is left
off a lane for time. The rest of the TSan difference is the failed
`k37_segment_rebuild` entry (154 s against 67 s when it passes).

## Findings

No ASan, UBSan, LSan or ThreadSanitizer report in any of the seven binaries.

One failure in an existing entry, not a sanitizer report and not in the
seven binaries: an intermittent not recorded before, cause not established,
so not yet classified as test-only or product:
`k37_segment_rebuild` in the first TSan after run, step 5 ("never on fewer
than two durable fragments"): after node 3 restarted, the packer moved 0 of
the 4 refused files within the step's 120 s wait
(`refused files once node 3 is back: packer moved 0 of 4`); node 1's packer
kept logging `backlog=0` while the files stayed unpacked, and the cluster
status was OK, 3 up. The entry passed in the before run (67 s), 3 of 3 solo
on the same TSan build (68, 68, 67 s) and in the second full TSan run. The
change adds lines to the lists only and the new entries run in their own
processes before it, so the failure is not caused by this change; whether
the packer can miss files refused while a member is down (a product
question) is left as a follow-up, with the failed run's output and daemon
logs kept (below). No K row: one occurrence, no cause.

## Raw output

node9900 `~/efs-artifacts/taskEM-20261109/` (`SHA256SUMS` sha256
`cf9efcaf072721da9a94c95515a66289ea36b717f5a342ce12ef73e3331d5cdf`): `before-{asan,tsan}.txt`, `after-{asan,tsan}.txt`,
`after2-tsan.txt`, `k37-solo-tsan.txt`, `threads.txt` (the thread check),
and `after-tsan-k37-fail/` (the entry's output and the three daemons' and
the mounts' text logs; stores not kept).

## Not done

- `make test` also runs `EFS_TEST_EARLY_READ_ACK=1 ./tests/test_raft`; only
  the plain binary is on the ASan list, as the brief asked.
- fstor007 not run (the brief named node9900).
- The k37 packer stall is not investigated here.
