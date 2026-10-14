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

## Live deployment and traced command lifecycle

`a5743754977de08ad58bebe58bc78f29d64f8fa5` is installed on all eleven hosts. Installed source proofs (884 inputs per host), nonroot/native RDMA runtime flags, direct/files daemon profile, seven command-enabled unpacked mounts and acknowledged-marker cold reads PASS. Four-daemon elected/caught-up readiness took 137.299 seconds. General full-size/400 GiB acceptance remains on hold; marker reads do not qualify every payload.

The human observed EBUSY on root `.efs/ls` and `.efs/stats`. The explicitly assigned NUC agent traced six commands successfully and found first cache publication at 04:09:36–40 UTC, after initial builds of 381–384 seconds. This supports an initial-publication explanation; no failing EBUSY command was captured, and the initial-publication gate is not closed by later success. The same investigation confirmed a separate cleanup defect: token `284626450853288382` on owner 3/fcstor005 could continue successfully; zero-limit close returned EIO after 6.7 ms; old-token continuation then failed; a fresh request using the same identity obtained a new token. Raw traces and lifecycle JSON remain under `/home/erbmi1/orcd/scratch/efs/orcd-test/fcstor007/query-ebusy-20261014`; lifecycle trace SHA256 `d1bb3a572a9226cd2d2058c41fd62e3433353f0955cd343fdc7fdf0294151e39`. Source analysis identifies repeated virtual LOOKUP/GETATTR/OPEN calls: the first close clears the session; the next acquisition reports STALE, mapped to EIO. The protocol status is source-derived, not a captured wire response. This sampled token was not retained; it does not prove that every reader retires correctly.

The commit adding this paragraph makes zero-limit close idempotent when the token is already gone, returns no data and allocates no session; live-session identity and BUSY checks remain, as does STALE for unknown-token continuation. Page dispatch guards a NULL session on the already-closed path. Production-code session and page-dispatch regressions PASS locally with ASan/UBSan (Mac leak detection unsupported; Linux leak check pending); both old-code negative controls fail as expected. Reproduce with `python3 tests/test_query_page_sessions.py` and `python3 tests/test_query_page_barriers.py`; use `CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer"` and supported sanitizer options. Final Linux qualification, redeployment of this correction and live lifecycle retest are NOTRUN. The seven-client 140,000-file finite close gate on `a5743754977d` FAILS its unchanged 1,800-second controller deadline: fcstor008/013 finished in 1,309/1,309 seconds; the other five finished later in 3,636–3,639 seconds. All seven eventually completed 20,000 files with zero reported data errors and exit 0. Late completion does not turn the deadline failure into a pass. All finite processes subsequently drained normally; sustained benchmarks remain stopped.

## Reviewed integration and outstanding loaded gate — October 14

Reviewed every local development branch against package 1. `devel-container`,
`devel-expiry`, `devel-efs` and `devel-kimi` have no missing commits;
`devel-rdma` is contained in reviewed `devel` endpoint `a40428d9d`. Merge
`b65d99dcf280f1a9919688997b65e1413c4d5af5` incorporates that endpoint while
retaining the query token-close correction `850168c66`. Production merged
cleanly; generated architecture HTML was regenerated, all five architecture
checks PASS. The superseded local pending-frame draft is preserved in Git stash,
not applied on top of the integrated fix.

Relevant production changes: `61e5c939d` preserves accepted EOF while a cached
body awaits publication; `a7e2b6b7d` routes unlink/link/rename before follower
coordination; `ade336daf` puts a stalled preferred fragment last within the
existing read deadline; `1ac603128` bounds queued ordered Raft data per peer/group
with flow control off, including the defensive enqueue guard. Dequeue reopens
admission; control frames and other peers/groups can progress. No durability
acknowledgment, operation deadline, transport pipeline or optional feature was
relaxed. Intel's deep-directory timeouts remain open. Prior AMD passes are
source-specific and do not qualify this merged ORCD deployment.

On merged `b65d99dcf`, local native regressions PASS: host pending entries, host
flow, pending EOF, namespace routing, read preference, query sessions and query
page barriers. Host pending entries, pending EOF and read preference also PASS
with local ASan/UBSan. Mac does not run the Linux async sender portion or qualify
Linux leak detection. Reproduce using `python3 tests/test_NAME.py`, where NAME
is `host_pending_entries`, `host_flow`, `inode_pending_eof`,
`namespace_leader_route`, `read_peer_order`, `query_page_sessions` or
`query_page_barriers`; for sanitizer runs prefix
`CC="cc -fsanitize=address,undefined -fno-omit-frame-pointer"`. Full Linux native,
Linux focused sanitizers and loaded mounted gates remain NOTRUN on the merge.

A second 45-second profile on installed `a5743754977d`, with five surviving
finite fsync workers, did not reproduce the earlier sustained global-mutex
convoy in the subsequent ten-second lock sample. Cache incremental publications
succeeded in roughly 0.75–1.16 seconds. A separate 200-sample/50-ms bounded
read-only outbox observation on fcstor005 found ordered data queued to fcstor003
at mean 506/max 767 frames and to fcstor004 at mean 62/max 86; priority lanes
were small. This supports testing the default pending-data admission fix, not
a claim of completed acceptance. Observer-disabled send/failure counters provide
no transport-failure verdict. Workloads differ from the original seven-writer/
seven-crawler profile, so throughput comparisons are not controlled.

Raw current profiles remain at
`/home/erbmi1/orcd/scratch/efs/orcd-test/fcstorNNN/live-profile-20261014-043729`;
finite logs are `close-eio-20000-a5743754977d.log` in each host's scratch directory.
Baseline remote perf/pidstat/process-I/O captures remain intact. An accidental
local filename reuse overwrote baseline-only ten-second lock JSON/iostat and
some post-build snippets; these cannot be reconstructed and are not claimed
as retained evidence. Current captures use a distinct source prefix.

Next: qualify the frozen merge on Linux, normally redeploy all eleven nodes
without erasing data, verify acknowledged cold markers and the exact unchanged
RDMA/direct/files/commands-on profile, then rerun seven simultaneous
`tests/live/fsync_admission.py /tmp/efs-mount --files 20000 --workers 16`
with the original 1,800-second deadline. Concurrent commands, idempotent live
close, and sustained seven tiny-file writers plus seven MD5 crawlers remain
required. General 400 GiB acceptance stays on hold.
