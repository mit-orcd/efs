# ORCD query cache name ordinals — qualification pending

Gate: [package 1, W91/W92/W108/W109](three-work-packages-20261013.md).

Purpose: correct the failed `.efs` refresh churn identified in [the live profile](orcd-live-profile-20261013.md), alongside the separate Raft mutex convoy investigation. This candidate is NOTRUN on ORCD; the installed source remains `e8bab21014bad21e5ea2198141e4df3a45d3dc3d`. Its parent is `3d9d72af7`, including the seven reviewed devel W100 commits. The exact candidate is the commit introducing this page.

The rebuildable cache changes manifest version 6 to 7. Persistent name order uses an eight-byte ordinal into its own immutable raw dentry family instead of a duplicated 288-byte name record. Full names, query ordering, cursors and authoritative Raft/KV data are preserved. Temporary sort runs still carry complete names and an ordinal, with two full temporary runs charged before writing. The 16 GiB generation and 32 GiB aggregate limits remain unchanged. Older v5/v6 current generations are retained until successful durable replacement; registered readers retain their generations.

At the profiled approximately 17.9 million names this removes approximately 5 GiB of duplicate persistent names. This estimate is not a measured live result or a promise that larger namespaces fit. The Raft-host mutex convoy and sustained close EIO remain open.

Qualification required: all six native query suites; failure injection and cancellation; actual temporary peak accounting and its negative control; ASan/UBSan; genuine version-6 cold migration, failed publication rollback and retirement; loaded concurrent queries and sustained seven-client close/fsync jobs exceeding the previous approximately 29-minute failure window. Reproduce native query checks with `make -j4 tests/test_query_catalog tests/test_query_disk tests/test_query_delta tests/test_query_disk_io tests/test_query_policy tests/test_query_refresh` and run each executable. Use a private Linux build and temporary test directories; no live stores are erased. Mac compilation is blocked by the existing Linux-only `fopencookie` logging implementation.

The deployment target remains ORCD-test, four native IB servers fcstor003–006 and seven clients fcstor007–013, RDMA/direct I/O, files storage, `.efs` enabled, packing/containers/expiry disabled. Live deployment and large-namespace verdict are NOTRUN. Keep profiler/build outputs outside Git under the existing scratch qualification paths.

## Private native results

On fcstor006, production query code from `aa88073c529a50b8b4acdddaa51306db676b740d` passed all six query suites after updating the old final-name rename fault hook to inject a compact-output write failure. This test-only correction is in the commit adding this paragraph. Real peak accounting and out-of-range ordinal refusal passed. Private raw logs are `/home/erbmi1/orcd/scratch/efs/orcd-test/fcstor006/query-v7-aa88073c529a-native.log`.

The cross-build oracle `tests/test_query_cache_migration.c` passed using actual `e8bab21014ba` and candidate libraries: 100 maximum-length file names, one nested directory, paginated complete result hash, rejection/preservation of the old checkpoint, durable replacement/retirement, then cold restoration. Compile it in each tree with `cc -D_GNU_SOURCE -std=c99 -Iinclude -Isrc/common -Ideps/blake3 tests/test_query_cache_migration.c libefs.a -lpthread -lm -ldl -libverbs -o tests/test_query_cache_migration`. In a newly created empty private root run `OLD_EXE create ROOT` followed by `NEW_EXE migrate ROOT`. Retain the fixture on failure; cleanup only the private root. The retained passing ORCD fixture is `/tmp/efs-query-v6-v7-a0V6YV` on fcstor006.

Sanitizer, exact final merged-source qualification and live deployment remain pending. These small private checks do not clear the sustained close EIO or Raft convoy gate.

## Combined candidate

`a5743754977de08ad58bebe58bc78f29d64f8fa5` includes reviewed devel through `382d537d6`, combining compact names with the completed-receive-batch host-mutex handoff. Full native `make -j4 test`, focused handoff ASan/UBSan and hardware IB segmented/transport gates PASS on fcstor006; all eleven staged hosts match 884 code/test input hashes. The six fully linked query sanitizer suites used the identical query production changes at `aa88073c5`, with the final-name write fault-hook correction. The old scratch-reservation omission negative control fails the physical file-byte oracle as intended. Live redeployment is in progress; loaded convoy, fresh catalogue and sustained close gates remain OPEN.

Preparing live command qualification found two harness defects in `efs_command_options.py`: it reused a fixed fixture name and redirected by incrementing ports on one host, whereas ORCD maps primary IDs to distinct physical hosts using the same port. Use a unique preserved fixture and the existing shared routed-query helper, which respects the supplied node map and preserves business refusals. This test-only correction is separate from the deployed binary identity. Run the six-suite wrapper against a private copy of this corrected test harness; retain the exact harness commit in the run summary. No operation deadlines or production protocol are changed.
