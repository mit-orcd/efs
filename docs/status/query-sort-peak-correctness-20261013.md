# W92 / W109: checked sort cleanup and temporary peak correctness

Scope: correctness and blocked ORCD acceptance only. No performance tuning,
new compression, cache-format change, larger budgets or relaxed deadlines.
Code commit `5f8ec77963bee0803cf8f127b800f48fb990b765` on `devel`.
Bounded Linux qualification **PASS**; full ORCD acceptance **OPEN / NOTRUN**.

## Change and admission contract

Previously, every name and alias was permanently charged for two copies,
including shards whose sorting had already finished or whose indexes were
reused. That reservation is not the simultaneous temporary peak. The candidate
now reserves final indexes once and adds one full shard-index allowance before
each name/alias sort. Existing reserved final space plus this allowance covers
all input runs and merge outputs, including partially written outputs. Input
runs are removed before progressing to the next merge pair; both removals and
all sort descriptor closes are checked. A failed close/removal prevents
publication and retains the extra reservation until candidate disposal.
Changed-ID sorting retains its existing original-plus-two-copy admission;
its close/removal failures now also block publication. A derived-row private
copy releases its extra reservation only after successful replacement or
confirmed removal of the temporary pathname.

Generation/aggregate defaults remain **16/32 GiB**. Aggregate accounting still
reconstructs retained generations from disk and charges shared links per
generation conservatively. Filesystem allocation overhead remains protected
by the existing free-space reserve; these caps and the measurements below are
logical file bytes, not allocated-block guarantees. Failed candidates do not
replace current, discard readers or grant budget exemptions. No record ABI or
manifest version changed.

## Exact qualification

Environment: Intel1, Linux/GCC, isolated native tree
`/data1/efs/evidence/query-peak-correctness-20261013`. No mounted command activation or
production deployment. Intel production remains ordinary files/TCP/direct,
`.efs` client/server and expiry disabled. Native synthetic
query fixtures exercise excluded live features without enabling them.
Source began at `328153c9`; all five changed code/test files were replaced by
the committed `5f8ec779` versions. Force rebuild used the identity below.

```sh
make -j8 -B EFS_GIT_ID=5f8ec779 EFS_GIT_DESCRIBE=5f8ec779 EFS_GIT_BRANCH=devel \
  tests/test_query_disk tests/test_query_disk_io tests/test_query_delta tests/test_query_refresh
./tests/test_query_disk_io
./tests/test_query_disk
./tests/test_query_delta
./tests/test_query_refresh
python3 tests/test_readindex_remote_pair.py
CC="gcc -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie" \
  ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/test_readindex_remote_pair.py
EFS_QUERY_DISK_MAX_MIB=4 EFS_QUERY_DISK_TOTAL_MIB=8 \
  EFS_QUERY_TEST_FILES=3000 EFS_QUERY_TEST_QUOTA=1 ./tests/test_query_disk
EFS_QUERY_DISK_MAX_MIB=5 EFS_QUERY_DISK_TOTAL_MIB=16 \
  EFS_QUERY_TEST_FILES=3000 ./tests/test_query_disk
```

All commands **PASS**. The 4 MiB test deliberately requires quota refusal;
it still refuses during raw copying, so this is not a claim that a genuinely
oversized generation now fits. An exploratory 4 MiB full-oracle invocation
without the refusal expectation FAILS for the same reason. At 5 MiB the full
3,000-file oracle passes. These private test overrides do not change live caps.

`test_query_disk_io` uses 4,113 rows per name/alias sort, three initial runs and
multiple merge passes. Exact temporary allowance admits; generation or
aggregate allowance one byte below it refuses before creating a run. Partial
ENOSPC, cancellation, failed input removal and failed close return errors and
retain reservations. Successful sort verifies every output row and releases
only temporary accounting. File sizes are measured after writes and at exit;
no measured pathname bytes exceed the reservation:

| Sort | Measured logical peak bytes | Admitted bytes | Samples |
| --- | ---: | ---: | ---: |
| Names | 2,615,868 | 2,714,580 | 18 |
| Aliases | 444,204 | 1,628,748 | 18 |

Other passing checks cover unchanged RAM/query answers, paths, rename,
restore/catch-up, abandoned candidate cancellation, failed checkpoint
publication preserving current, retained readers after replacement, retirement
only after final reader release and a following refresh under a tight unchanged
aggregate allowance. Large-delta and refresh retry/coalescing regressions pass.

ASan/UBSan **PASS** with leak detection and halt-on-undefined enabled:

```sh
gcc -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -O1 -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
  -Iinclude -Isrc/common -Ideps/blake3 tests/test_query_disk_io.c libefs.a \
  -lpthread -lm -ldl -libverbs -o /tmp/query-peak-io-asan
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 /tmp/query-peak-io-asan
```

This instruments the embedded production query-disk implementation and test;
linked library dependencies are the normal build, not a fully instrumented
product. An initial manual compile omitted GNU feature macros and failed before
execution; the complete corrected command above passes.

Negative control **PASS**: with only `query_disk.c` and
`query_disk_incremental.h` restored from `328153c9`, the new I/O fixture exits1
with19 assertions failing (missing quota/cleanup refusal and changed temporary
reservation contract). Reproduction: use a separate `328153c9` checkout, copy
`tests/test_query_disk_io.c` from `5f8ec779`, build and run that target; expect
nonzero. Do not alter a production tree. Test-created `/tmp/efs-query-*` stores
are private; remove only those owned fixtures after no test process remains.

Built normal I/O test SHA256:
`2bb5a3c31607002904ca091640ae9f0a23c1a8d172a5ce46157c292df290738e`.
Query source SHA256:
`3dee086461116a91cba11a360e76a213aa59c5590c3131c34e6e7111288deb30`;
incremental header:
`ae5a519ff785bb73f7af55ad09a2b5bde01a5e813fb1dd57f178f3d8efbf0bc9`.
Raw evidence remains outside Git on Intel1:
`/data1/efs/query-peak-{build-final,io-final,disk-final,delta-final,refresh-final,helper-rollback,helper-asan,quota,5mib,asan,negative}.log`.
Reproduction requires Linux build dependencies, not these private captures.

## ORCD delivery and remaining acceptance

The committed fix is ready for acceptance on the full ORCD namespace with the
existing 16/32 GiB limits. Full-size rebuild/refresh, six focused mounted command
suites, concurrent queries, retained paging/readers and next refresh, followed
by same-source cold readiness and complete acknowledged payload verification
remain **NOTRUN for this fix**. Preserve the native RDMA/direct/files profile,
`.efs` client/server on, expiry off. Native correctness and
peak evidence do not replace these mounted hardware/scale gates or establish
that ORCD's observed ENOSPC is fully resolved. If refusal remains, record
actual retained/final/temporary peak bytes before changing code; no further
compression, tuning or budget increases are authorized by this scope.

The Intel 32-helper experiment was withdrawn in this same code commit. Its
already-running native checks finished on both hosts, but it was never deployed.
Ordinary-file helper capacity is restored to8; related normal/sanitizer fixture
variants pass. Further Intel performance experiments are paused by human scope.
Both Intel hosts were rechecked with `scripts/intel_package3_verify.py`: actual
process/profile, mounts and disabled server command-policy gates PASS.
No new production deployment or enabled-command workload was started.

**Raw evidence note (2026-10-23):** the bulk data in `query-peak-correctness-20261013` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
