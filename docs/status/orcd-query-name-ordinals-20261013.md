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

## Merged Linux qualification and deployment

Exact production source `b65d99dcf280f1a9919688997b65e1413c4d5af5`: full Linux
`make -j4 test` PASS on fcstor006; seven focused regressions listed above PASS
with ASan/UBSan and leak detection on fcstor003, including the Linux asynchronous
sender. The first sanitizer invocation failed because host-flow treats CC as an
executable rather than a shell command. The second run used a compiler wrapper
with sanitizer flags; no source fix or relaxed check was involved. Logs are
`fcstor006/package1-full-native-b65d99dcf280.log` and
`fcstor003/package1-focused-san-b65d99dcf280-r2.log` under the ORCD scratch base.

All eleven installed hosts match 890 committed code/test inputs. Runtime
executable hashes, UID246111, native mlx5_2 verbs descriptors, four files/direct
daemons, seven command-enabled unpacked mounts and disabled expiry/pressure/
flow experiments PASS. Four-daemon elected/caught-up readiness took 126.871
seconds. Acknowledged large and 512-byte markers captured before the normal
stop read back with exact SHA256 on all seven remounted clients. No store was
erased or forcibly unmounted. This qualifies these markers, not the full
400 GiB payload inventory, which stays deferred under the existing hold.

Mounted close regression `c51f98d2f21f86a32a9db8c0dae812983e813287` ran privately
on fcstor007 against this deployment and PASS: three read-only opens of the same
zero-limit close returned empty bytes in 0.573/0.516/0.505 ms, old-token
continuation returned the expected mapped STALE/EIO, and a fresh request with
the same identity acquired a different token and closed successfully. Reproduce
with `python3 tests/live/query_page_close.py /tmp/efs-mount`, using its committed
production helper encoder. No raw TCP RPC or fixture writes are used by this
oracle. Token-close behavior is now verified live; loaded retained-reader and
concurrent-query gates remain distinct.

The new seven-client finite gate is RUNNING with 20,000 files and 16 workers per
client, keeping the original 1,800-second controller deadline. Original tiny-file
and MD5 screen jobs remain stopped until these gates pass. No throughput or
sustained close-EIO acceptance is claimed yet.

## Loaded mounted results and follow-up correction

On installed `b65d99dcf`, the finite seven-client gate PASS: all 140,000 files
complete with zero reported errors/exit0 in 491.509–492.969 seconds per client,
within the unchanged1,800-second limit. Each job checks create/write/fsync/close,
exact32-byte EOF/content and unlink. Concurrent extras/virt_ query cases
PASS21/21 (seven clients, three jobs/client,30-second case budgets) while the
finite jobs were active. A bounded read-only200-sample/50-ms queue observation
found at most one queued ordered frame per peer across all four daemons; it
does not prove every future queue or latency outcome.

Broader two-independent-mount command qualification FAIL5/6:
`efs_commands` passes; `efs_command_options` returns BUSY on its raw filtered
request after mounted filters/formatting pass; `efs_command_pages` and
`efs_commands_load` encounter NOT_PRIMARY with target0; `virtual_interfaces`
fails the raw supplementary-group permission gate with NOT_PRIMARY;
`query_large_delta` verifies both initial1,025-file payload phases, then ordinary
readdir returns EBUSY on the auxiliary mount. Failed fixtures and all suite logs
remain at `fcstor007/six-commands-b65d99dcf280` under the scratch base. The
auxiliary mount subsequently drained and unmounted normally (cleanup exit0).
No failing case is silently reclassified as a pass. Original seven screen
writers/crawlers remain stopped pending the open gates.

The retained options fixture `efs-options-gates-1791955799554925371` was probed
quietly after the finite workers drained: five identical raw filtered requests
passed in1.7–3.8ms,851-byte replies. This is diagnostic evidence of loaded
refusal, not replacement qualification of the earlier failed suite. The raw
wire acceptance harness uses TCP control RPCs; mounted filesystem operations
remain strict hardware RDMA/direct.

The next candidate normalizes a query NOT_PRIMARY refusal to BUSY only when
no usable primary target accompanies it, at early policy and final reply
boundaries. Known targets preserve redirects; failed proofs still emit no
bytes, retain existing release/accounting rules and never gain authority.
Actual page-dispatch regression covers missing-target continuation, known-target
fresh redirect, early policy NOT_PRIMARY/ACCES and existing refusal/close
paths. Local native and ASan/UBSan PASS for the page-dispatch change; Linux
and live checks on this new candidate are NOTRUN. The supplementary-group
harness now uses routed_query instead of a seed-only raw client, retaining
its credential and expected permission assertions.

Intel was given the ordinary-core readdir failure for its remaining package3
work on Intel; AMD was given the query-refusal evidence for package2 coordination.
Root owns the reply normalization draft to avoid duplicate edits. No workload
deadline, quorum, feature mask, cache limit or data-plane transport was changed.

Queue observations can be reproduced without private controller files using
`scripts/orcd_queue_sample.py --node fcstor005 --revision FULL_INSTALLED_SHA
--seconds 10 --output /tmp/orcd-queue-sample.json`. Supply `--ssh-control PATH`
when reusing an authenticated login master. The script validates source/UID/exe,
reads gdb types without attaching, accounts for PIE, and samples only the
ordered/priority counters and two pending-group counters through read-only
process memory. Never interpret these racy samples as a durability proof; keep
raw output outside Git.

## Follow-up live result and internal retry normalization

`f58eab0f1d99d0158c93a12d706cbebef8bd5ccc` full Linux native, focused
ASan/UBSan/leaks and893-input installed source proofs PASS. All11 runtime flags
match the required profile; normal marker restart PASS, elected/caught-up
readiness132.883s. Repeated mounted close PASS again at0.53–0.58ms.

The quiet six-suite rerun improves to5/6 PASS: basic5.52s, options5.87s,
pages6.67s, virtual interfaces49.71s, large-delta21.24s. Large-delta includes
the previously failed peer readdir after rename/link/512 unlinks. This quiet
pass does not invalidate the earlier loaded EBUSY failure. Load remains FAIL
14.08s: reader receives internal retry status EFS_ERR_AGAIN(-18), not the
BUSY refusal its unchanged oracle accepts. Successful mounted snapshots show
advancing generations79–82 and refresh_error0 before the failure. The auxiliary
mount drains/unmounts normally, cleanup0. Logs are retained separately at
`fcstor007/six-commands-f58eab0f1d99` under the scratch base.

The next candidate maps internal AGAIN to query BUSY, matching the existing
ordinary-inode rc_to_inode_status behavior. It retains known redirects, errors
such as ACCES/IO, reply accounting, failed-proof zero bytes and existing stream
release behavior. Actual page-dispatch tests inject AGAIN in early policy and
consume paths and require BUSY without data; the load oracle is unchanged and
still requires successful reads and snapshot progress. Local native/sanitizer
checks PASS; Linux/live qualification for this additional correction NOTRUN.
All seven long-running writer/crawler screens remain stopped.

## Final production source and page-oracle freshness window

Production `90a1324d10ea8861127083627ddfbb131988fa71` passes full Linux native
and focused ASan/UBSan/leaks, with894 input hashes verified on all installed
hosts. A staging diagnostic defect printed branch devel although the frozen
source SHAs were correct. Before deploying90, only version.o was rebuilt and
binaries relinked to print devel-orcd-package1; all11 efsd/efs-fuse --version
checks passed. Behavior code is unchanged by this text correction; final
version-display relink hashes are retained with the deployment metadata.
Required runtime flags/native verbs, normal restart and acknowledged marker
reads PASS.

The unchanged six-suite run on90 PASS5/6. Load now PASS: two readers each have
299 successes across three advancing generations, all2,761 payloads exact;
writer p50/p95/p99=33.74/68.79/87.95ms, final convergence2.37s. Query p95 about
54–55ms and p99 about115–119ms; temporary BUSY refusals are explicitly counted
by the original load oracle and carry no bytes. Pages FAIL on a one-shot raw
first-page BUSY after its mounted complete/NUL/NDJSON export checks pass.
This failure remains recorded at `fcstor007/six-commands-90a1324d10ea`.

The following test-only correction makes positive raw-page requests respect
the same eight-second freshness window as mounted helpers. Only BUSY is
retried; hard statuses and negative probes are unchanged. Redirects/socket
reads share one absolute deadline, with each socket retaining its at-most
seven-second timeout. The240-second suite budget and production query deadlines
are unchanged. `tests/test_query_rpc_retry.py` verifies late publication beyond
eight fast refusals, unchanged request identity, exact hard refusals, remaining
route time and rejection of partial BUSY bytes. All four local checks and
34 production helper tests PASS. Private Linux and live corrected-harness
qualification remain pending. Installed production source stays90; record the
separate harness SHA when qualifying this correction.

## Dual-query target starvation — controlled diagnostic

Private corrected harness93 on production90 passes basic/options/pages/virtual
checks but a further complete run FAILS load and large-delta. Load readers have
0 successes (162/159 BUSY replies, no bytes); large-delta again reaches peer
readdir EBUSY after both1,025-file payload phases. Earlier isolated passes
remain historical and do not close this repeated failure. Private staging
initially lacked the efs-mgmt path; linking the verified90 binary fixes that
staging failure. A subsequent wrapper path typo runs no suites and is NOTRUN,
not a production regression. Normal auxiliary cleanup succeeds on every attempt.

A controlled14-second, eight-writer comparison uses the same load fixture and
one raw reader per dual-group node. fcstor004 (both groups FOLLOWER) has0/187
successes. fcstor005 (group0 LEADER, group2 FOLLOWER) has333 successes across
five generations; its first successful headers carry its boot/generation and
10 checked directories. Sampled BUSY headers havezero boot/generation/checked
directories andzero data, supporting pre-consume refusal rather than filtered
result corruption. Both see the same writer fixture; this diagnostic does not
qualify its final payload oracle, which aborts on the zero-success reader.
Raw output: `fcstor007/query-target-20261014-065406.log` (the controller records
the actual scratch path), plus the separate header comparison controller output.
Record the retained path from the run when reproducing; no private captures are
needed to reproduce the hypothesis using the committed14-second load driver
with one reader explicitly addressed to each dual host.

The next candidate changes only a non-capable query host's redirect selection:
prefer a known leader whose committed voter membership covers both groups,
then retain the original dual-host fallback if no such leader is known.
Capable local hosts still serve; fresh authority, import guards, permissions,
retained generations and pinned continuations are unchanged. This avoids the
seed always choosing the first dual follower when a known eligible leader is
available. Existing namespace leader-cover selection provides the membership
check; it grants no authority.

Actual production-function target regression PASS locally/native+ASan/UBSan:
dual leader preference for either group, single-group/unknown/removed leader
fallback, local-capable service and BUSY/IO/changed-capability refusal. The
actual old90 source compiles and fails the target assertion at runtime (expected
SIGABRT). Reproduce with `python3 tests/test_query_target_route.py`; use
`EFS_RAFT_HOST_SOURCE` to select the old source for the negative. The first
negative invocation had an unused helper warning; explicitly testing that
helper makes the intended old-code negative compile and reach the runtime
assertion. Linux/live qualification of this new routing candidate is NOTRUN.

Committed diagnostic reproduction: run `python3 tests/live/efs_query_target_compare.py
/tmp/efs-mount /tmp/INDEPENDENT_RDMA_PEER_MOUNT --targets 172.16.223.58
172.16.223.59 --port 17432 --fixture UNIQUE_UNUSED_NAME` on a client. Redirect stdout/stderr to an owned scratch log
outside Git. Both mounts must already serve the same export under the required
profile. It uses the stock14s/eight-writer workload and payload checks, changing
only reader endpoints and adding bounded header samples; it retains the original
zero-success assertion. Stop the auxiliary mount normally only after the
driver's threads exit, preserving failed fixtures.

Linux focused sanitizer qualification on routing source3fc04d2c1 PASS, including
actual target selection and bounded positive-page retries. Full native FAILS
its raw-query routing test adapter because that adapter lacks the newly
explicit timeout keyword. Update the adapter to assert the unchanged default7s
while retaining all endpoint/identity/lease-owner assertions. This is test-only;
requalify the combined frozen tree before deployment. Production still90.

Frozen combined sourcefa0ac827f passes clean full Linux `make -j4 test` and
the targeted ASan/UBSan/leak suite, including target selection, raw endpoint
routing and the four bounded freshness checks. All11 staged hosts match897
code/test inputs. Full native log is `fcstor006/package1-full-native-fa0ac827fef6.log`
and focused sanitizer log is `fcstor003/package1-focused-san-fa0ac827fef6-r2.log`
under the scratch base. This supersedes the test-adapter failure on3fc but
does not qualify live workload results yet. Normal all11 deployment is in
progress, same RDMA/direct/files/commands-on profile, optional features off,
with no store erase.

## Concurrent synthetic-inode reservation

On installedfa0ac827f, all six command suites PASS and the direct seed probe
returns NOT_PRIMARY/target3/zero bytes, as intended. The repeated seven-client
finite gate also PASS140,000 exact files/zero errors in390.59–573.46s per
client, within1,800s. The simultaneous21-query cohort FAIL20/21: fcstor007
virt_find_query gets ENOENT opening its v3 synthetic query inode; the other
six clients pass3/3. Refresh error remains0. Retain the failed case and TSV
`fcstor007/query-concurrent-fa0ac827fef6` under the scratch base.

Source review finds a deterministic virtual-cache race: vq_intern releases
the64-slot cache mutex before lookup_fill completes getattr and adds a lookup
reference. Another intern can recycle that still-unreferenced mapping and
getattr/open then sees ENOENT. This can explain the observed symptom; the
precise live failing instruction was not traced, so it remains a causal
hypothesis rather than a captured stack.

The following candidate reserves the mapping while interning, excludes
reserved entries from recycling, and releases the temporary reservation after
lookup_fill on both success and failure. Successful replies already hold
their kernel lookup reference; failed getattr leaves no reservation. Both
helper-script and result-inode call sites balance the reservation. The64-slot
limit, open/lookup pins, immutable bytes, FORGET and invalidation semantics
remain unchanged.

Actual-cache regression PASS native/local ASan/UBSan:63 pinned old identities
plus the new pending intern cannot recycle it; release after failed lookup
permits reuse; successful lookup transfers to its kernel reference, and
FORGET permits later recycling. Real oldfa0 source compiles and fails the
racing-intern assertion at runtime as expected. Reproduce with
`python3 tests/test_virtual_export_handles.py`, optional sanitizer CC, or
`EFS_FUSE_SOURCE=OLD_SOURCE` for the negative. Update the LOOKUP error test
adapter with the new release hook, keeping every errno assertion. Linux/live
qualification of this client correction is NOTRUN; sustained screens remain
stopped.
