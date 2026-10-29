# Task T — directory visibility during cross-group migration

**Isolated TCP qualification PASS; full-gate qualification in progress**, on
`agent/devel-task-t`; no merge or push.
This is a new counterexample relevant to [K48](k48-readdir-omission-20261028.md)
and [Task T's full-gate measurements](gate-budgets-per-host-20261028.md).
The user explicitly authorized fixing and qualifying it in this branch.
The current candidate is `35a6e41b042e84982268f8569cc31ddc739be5e1`.
Its deterministic controls, three K48 runs, ten W130 runs and B6 refuse
pass in the isolated scope below. Fresh full measurements are still running;
no complete full-gate qualification is claimed.

## Discovery and preserved evidence

Xorinox's first `b13b8867e` eight-job gate finished 547 commands, **2 FAIL,
10 NOTRUN**, 953 s, two budget overruns, build 24 s. It stopped before the
next measurement. W130's post-resync listing returned 95 of 96 names,
missing `f0057`; its earlier healthy-quorum listing had passed. The separate
B6 refuse failure concerns counter accounting, described below. Neither
failure is accepted as calibration input or retried into a passing result.

Canonical capture:
`xorinox:/data1/efs/evidence/taskT-20261028/xorinox-b13b8867e-calibration17/`.
Its 78-file manifest SHA-256 is
`71d3cf1c00ddaefb42e222fc503961d304ee23e39658070f37f691014a7476fa`.
The original stores remain at
`/home/maik/efs-taskT-20261028-b13b8867e-calibration17/tmp/case-507/efs-w130-ifo92zq4/w130`.
All 456 original files were hashed before inspection and verified unchanged
afterward. Only disposable on-host copies were opened with the exact source's
`scripts/forensics/k48_store_scan.c`; opening an LSM can recover its WAL.

All three copied KVs contain target directory inode 855 and `f0057`, inode
238423. Static READDIR returns all 96 names at page sizes 1, 2, 4, 8, 16,
32, 64 and 128. The directory remains SPLITTING, with all lane bits set.
This establishes no persistent dentry loss in those final stores. It does
not establish the exact historical RPC/transaction interleaving: that trace
was not captured. No full-payload verification claim follows from metadata
inspection.

## Two demonstrated gaps and their correction

The deterministic regression extracts the production
`host_dir_migrate_txn` function and executes its PREPARE operations against
the real transaction layer and READDIR over fresh memory KVs. Its transport
adapter pauses between participant RESOLVEs; it does not substitute a listing
implementation or read filter.

1. **Source resolves first.** Previously the transaction deleted the local
   row and inserted its hashed replacement on separate participants. After
   the source DEL resolved but before the destination PUT, raw READDIR saw
   95 names; after both resolved it saw 96. This is a proven counterexample,
   independently of attribution of the captured incident.
2. **Destination resolves first.** Retaining the source closes that gap,
   but a newly used lane has a second cut: the hashed copy can materialize
   before the parent bitmap's OR. READDIR suppressed the local name without
   visiting the unregistered hashed source. A stronger fixture, with the
   migrated name on unused lane 1 and all later hashed names on lane 63,
   reproduces 95/96 on candidate `40ef229c6`.

The current protocol is copy, then cleanup. The copy transaction keeps the
local row and inserts the hashed copy. Its source uses an exact-byte CAS;
its destination requires absence. This prevents stale captures from restoring
a deleted source or overwriting a new hashed row/tombstone. The existing DIR
cleanup returns BUSY while either key has a pending transaction, preventing
a delayed source PUT from recreating a cleaned-up leftover. READDIR defers a
local live name only if its captured bitmap includes the hashed lane. Hashed
tombstones still dominate immediately. The final layout/bitmap recheck and
new-group ReadIndex proof rules remain in force.

No format, wire encoding, user switch or deadline changes. The cost is one
additional existing cleanup command for a cross-group move. Fresh full gates
must measure that cost. This does not repair the separate restart-time split
queue rediscovery issue or close the original K48 incident by attribution.
The [directory protocol](../how-it-works/protocols/directory.md) records the
current copy/cleanup rule.

## Controls and qualification

Current source: `35a6e41b042e84982268f8569cc31ddc739be5e1`, archive SHA-256
`6acc01fac0c03f7d26b5e2d68359647804a515bcd7eb7273560a115bae12401e`.
Product commits are `dabcf937a` and `35a6e41b0`; the test's deliberate
negative controls disable only their own core dumps (`40ef229c6`).
The old implementations are tested with their own libraries over fresh memory
KVs, never by opening preserved stores or mixing builds in a cluster.

The regression covers both resolve orders, COMMIT and ABORT, duplicate
resolve, cleanup while intents remain, a dominating tombstone, stale source
deletion and a newly installed destination tombstone. Current controls pass.
The original source reproduces the source-first gap, and the first candidate
fails the destination-first case. Both are expected negative-control outcomes,
not product PASS labels.

Xorinox: Ryzen 9 7940HS, 16 logical CPUs, GCC 14.3.1, XFS home filesystem,
FUSE 3.16.2. Live fixtures use three private loopback daemons, fresh stores,
free reserved ports, TCP, mandatory O_DIRECT fragment storage, background
packing at 500 ms minimum age / 200 ms interval, and the tests' normal quotas.
The malformed-row and READDIR pause hooks exist only in the fault daemon.
No existing deployment, other agent's campaign or Task AF was changed.

Finalized isolated raw capture (270 files, manifest SHA-256
`b2d4f965e2cf43e05bdb203de29ec5de86189b35ba1ff927fa80eb3338a70a4e`):
`/data1/efs/evidence/taskT-20261028/xorinox-35a6e41b0-directory-focus19/`.
All 24 expected outcomes passed: build, deterministic/current and negative
controls, metadata and transaction units, READDIR cursor/reply controls,
recipe checks, three K48 live runs (8.622/11.690/10.324 s), ten W130 runs
(21.282–33.197 s) and B6 refuse (34.976 s). Source hashes are unchanged,
there is no contention marker, and closure found no owned EFS process or
mount. Finalized B6 and W130 logs are retained. Its immutable source root is
`/home/maik/efs-taskT-20261028-35a6e41b0-calibration19/src`.
The full measurement controller on that root started only after isolated
closure proved no owned process/mount. It never overlapped the isolated
controller.
Fresh three-run full measurements use the same candidate on intel1,
xorinox and fstor007; their verdict belongs on Task T's evidence page.

The first candidate's partial qualification is retained separately at
`/data1/efs/evidence/taskT-20261028/xorinox-40ef229c6-directory-qualify18/`,
185-file manifest SHA-256
`8e667435aa926b26125b0cffd97583ce2ca33923be047bd3eef32dc7db85213d`.
It passed its weaker unit, three K48 runs and five completed W130 runs.
The controller was intentionally interrupted to strengthen coverage; its
sixth child finished normally and printed PASS, but its exit was not collected,
so it is not counted as a completed qualification step. A subsequent stronger
probe failed the destination-first cut. The capture is **not accepted**.
Its interruption-time leftover marker is kept; final checks prove no owned
EFS process or mount and unchanged tracked source.

## Reproduction

Use a quiet supported Linux host with FUSE, a fresh source checkout/archive,
and a writable private TMPDIR. Build and run from that exact revision:

```sh
make -j8 test-bins
python3 tests/test_dir_migrate_visibility.py
./tests/test_meta_apply
./tests/test_txn
python3 tests/test_readdir_source_cursor.py
python3 tests/test_readdir_reply_boundary.py
python3 tests/test_k48_readdir.py
W130_EVIDENCE_DIR=/data1/efs/evidence/NEW_RUN/w130 python3 tests/test_w130_dir_fault.py
EFS_B6_MODES=refuse EFS_B6_ARTIFACTS=/data1/efs/evidence/NEW_RUN/b6 python3 tests/test_b6_unavailable.py
EFS_TEST_TARGET=test-all EFS_BUILD_JOBS=8 EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh
```

Run K48 three times and W130 ten times serially with a separate private temp
and evidence directory per invocation; stop on an unexpected failure. Run the
full gate three times per measurement host. For negative controls, point the
current regression's `--root` at the pinned earlier source and its matching
`libefs.a`; `--expect-gap` must reproduce 95 names on `b13b8867e`, while the
default full-visibility assertion must fail on either earlier implementation.
The retained controllers record exact commands, environment, build identities,
raw outputs, source hashes and contention observations.

Tests stop their own mounts and daemons. Preserve failed fixtures. Finalize
and verify text captures before pruning closed successful bulk with
`~/bin/edelete --force --delete`; never remove original investigation stores
as part of a successful control's cleanup.

## Separate B6 counter-cohort correction

The original refuse log has a degraded-segment mark at 12:03:06.325 UTC and
its pass-end `packed_total=8` line at 12:03:06.337. The failed assertion reports
four new packed files and no new degraded segment. The counter/mark baseline
is inferred to straddle that pass; its exact snapshot was not serialized.
Replaying the retained log at that boundary reproduces the old false failure.

`780b6caba` cuts each process run at its last complete pack-pass summary, so
both observations enter the same cohort. The packer runs group passes serially
and joins segment workers before that line. Partial summary lines and process
counter resets are covered by `tests/test_b6_log_accounting.py`; the unchanged
logic fails the intended counter-cohort assertion. No read/write, degraded
protection, ENOSPC, payload or restart assertion was relaxed.

Three fresh focused refuse runs on xorinox passed in 36.132, 35.458 and
40.473 s, each checking 22 acknowledged files after restore and whole-cluster
orderly restart. Each observed one segment written degraded and 4, 4 and 2
files packed during the fault. These use the new harness and hash-verified,
unchanged `b13b8867e` product binaries. Canonical 32-file capture:
`/data1/efs/evidence/taskT-20261028/xorinox-780b6caba-focus-b6/`, manifest
SHA-256 `1644a1b310fe6a5400e568d91ced6eda13f0e7c8c6ea3bfafcf6bb30de9eae62`.
This is scoped B6 harness qualification, not a passing full gate.
