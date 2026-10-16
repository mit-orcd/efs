# Structural code review

**Release update:** [Branch revalidation and release disposition](release-disposition-20261011.md) supersedes removal recommendations for newer query consumers. Counts below describe the original pinned base.

October 11, 2026. Extended review of duplicated logic, parallel implementations, compatibility layers and dependencies of unnecessary code.

Review branch: `devel-code-review`; pinned integrated base: `48a2719d19fb5a6b0464935c70a967a1f1a86236`. This supplements [the initial dead-code and test-data report](dead-unused-code-20261011.md). All locations and counts refer to that base. No production source was changed. Temporary copies and extracted-function probes were used to test selected conclusions.

## Findings that change the cleanup assessment

There is more opportunity than the initial list of unused functions showed. The strongest larger candidate is the client cache's recursive directory statistics: substantial bookkeeping survives behind a formatter that no longer has a caller, while the current stats endpoint uses a separate server catalog. There are also small retired modes maintained through several functions, and repeated live utility code that can be consolidated.

The larger parallel implementations are a maintenance concern, but their entire line counts are not deletion estimates. Namespace planners, client write paths and staged publication machinery have consumers or documented integration work. Removing them without replacing their obligations would remove behavior or coverage.

| ID | Candidate | Evidence / disposition | Estimated net physical reduction |
|---|---|---|---:|
| S1 | Recursive client-cache stats and deferred rollup queue | Likely superseded; preserve immediate child counts and wire layout | 350–450 |
| S2 | Pipelined flush `drop_body` mode | Constant false in all production call paths; one test manufactures the old mode | 25–35 |
| S3 | Disabled permission mode in benchmark path walker | Only entry point passes false; private access helper supports that mode alone | 45–65 |
| S4 | Reserved inode-field behavior and dead inode allocator stores | No current producer of nonzero reserved state; preserve reserved wire fields | 35–55 |
| S5 | Duplicate cache patch primitives | Same executable work except trace label | 20–25 |
| S6 | Four profiling launcher/stop implementations | Live duplication; keep lifecycle differences explicit | 150–200 |
| S7 | Six address parsers | Five identical, one stricter; choose one checked contract | 32–42 after overlap with R10 |
| S8 | Repeated big-endian integer helpers | 62 bodies / 361 physical lines; preserve each byte format | 250–300 |
| S9 | Direct-KV link/rename implementation alongside transactional paths | Tests use it; migration/API decision required | Up to 395 gross, net not established |
| S10 | Serial and pooled client writes | Both live; share chunk/PUT and map-commit logic | Not included; design/prototype needed |
| S11 | Daemon and simulator namespace planners | Large mirrored domain logic; retain scheduling and independent oracles | Not included; design/prototype needed |
| S12 | Typed publication, tickets and lane state | Staged with explicit D25/W72/W86 plans and tests | Excluded from deletion estimate |
| S13 | RAM and disk query backends | Selectable comparison backend and differential oracle; keep independent checks | Excluded; production-boundary decision |

S1–S8 total **907–1,172 additional lines**, before any overlapping restructuring among those areas. Including the initial bounded 342-line candidate list gives **1,249–1,514 lines** as a planning range. This is not an approved patch size: utility estimates allow for replacement helpers, and S1–S3 have measured scratch reductions. R10 retirement, public API consumers and wire compatibility still require decisions. S9–S13 are deliberately excluded from the total. Avoid selling a 2,800-line namespace surface as 2,800 removable lines.

## S1 — Client-cache recursive statistics no longer serve the current endpoint

**Source:** `src/meta/metadata.c:1257–1669`; unused formatter at `:1671–1702`; `include/efs/metadata.h:91–93`, `:129–131`, `:228–237`.

Twenty function bodies in the rollup section occupy 384 physical lines. They compute immediate and recursive byte totals and time extrema, walk parent chains, rescan children after removal, and accumulate deferred changes in three dynamically grown arrays. Ordinary inode updates, hardlink synchronization and namespace operations call into this machinery.

The current `.efs/stats` path at `src/client/efs_fuse.c:232–239` submits a server query snapshot. It does not render these local fields. The server's query catalog/disk code owns a separate `efs_query_rollup` representation. `stat_to_inode` at `src/server/raft_host.c:5478–5496` clears the returned inode and fills ordinary attributes and allocation count; it does not populate local tree/time rollups. Searches found no current executable reader of the cache's recursive bytes/counts/time extrema outside metadata bookkeeping/copying and the uncalled formatter.

**Important boundary:** `imm_files` and `imm_dirs` are still read by `efs_inode_dir_is_spread` and `efs_export_dir_is_spread` at `metadata.c:3836–3847`. That decision influences cache name routing at `:1869`, `:2336`, and `:2529`. Keep these immediate counters or replace their routing role deliberately. Also keep `parent_touch` at `:518–527`, directory `nlink` updates, ordinary file timestamps, hardlink attribute propagation and cache indexes.

**Scratch experiment:** replaced the 413-line rollup section with immediate-child add/subtract/reset helpers and inert compatibility entry points, then removed the same-directory rename's recursive time expansion. The scratch file was **386 physical lines smaller**. Public structures and their wire size were unchanged. Existing `test_stage_evict` passed against both the original and scratch metadata implementation. A separate probe passed against both for create, hardlink, deferred size/mtime update, cross-directory rename and unlink, checking immediate counts and directory link count. This establishes that a sizable reduction is plausible, not that the scratch patch is production-ready. It leaves unused copy/state/call sites in place, deliberately.

**Follow-up implementation:** remove the unused formatter with this chain; replace rollup add/sub with clearly named immediate-child bookkeeping; eliminate deferred size/time queues and `*_norollup` branching once consumers are checked; remove saved/restored tree/time fields in hardlink copying. Keep public inode fields reserved/zeroed until a separate ABI decision. Do not shrink native RPC structs incidentally.

**Private row layout:** a separate layout-only probe measured `efs_inode_mem` at 200 bytes on the local 64-bit ABI, 136 bytes after removing the eight recursive/byte/time rollup fields while retaining immediate counters, and 120 bytes if the private reserved fields are also retired. Row slabs allocate `EFS_INO_SLAB_ROWS * sizeof(efs_inode_mem)`, so these fields affect row payload allocation. This is a potential 64-byte reduction per row for S1, or 80 bytes for S1+S4; names, indexes, chunk state and other cache allocations remain. The behavioral scratch test kept the old structs and does not validate this layout change. Public `efs_inode` must keep its native wire layout separately.

**Extra risk in the old chain:** `pending_rollup_note` at `:1609–1617` reallocates three arrays before updating their owners. On partial allocation failure it frees successful new allocations but leaves old owner pointers unchanged. Once an old allocation has moved, those owner pointers are dangling. An isolated probe extracted the exact function, forced the first/third reallocations to move and the second to fail, then performed ordinary owner cleanup. AddressSanitizer reported a double-free, as expected from the dangling owner pointers. This is a deterministic allocation-failure demonstration, not a live-cluster crash diagnosis. Fix this error path if the queue remains; removing the queue removes the failure mode.

**Accounting cost:** the three pending arrays add 17 payload bytes per capacity slot (8-byte inode, 8-byte delta, 1-byte flag), but `export_staged_bytes_one` (`:3578–3601`) does not include them. Clearing pending work resets the count, not the retained capacity. A baseline probe observed the first 64-slot allocation add 1,088 array payload bytes while `efs_export_staged_bytes` remained unchanged at 92,608 bytes. This does not establish an observed RSS overrun, but it shows that obsolete bookkeeping also bypasses this cache estimate. Remove the arrays with the chain, or account/bound them if they remain.

**Required before merge:** cache routing on wide/spread directories, create/link/unlink/rename and hardlink attribute tests, client memory gates, mounted stats compared with the server oracle, Linux builds, and external consumers of the public metadata API. No claim of a measured performance improvement is made here.

## S2 — A retired flush mode is threaded through a live pipeline

**Source:** `src/client/write.c:5536–5823`, production callers near `:6180–6191` and `:6812–6875`.

`dcache_steal_dirty_budgeted` writes `*drop_body = 0` and never writes one. Its wrapper passes that result through; both production callers initialize it to zero. `flush_pipe_add` records it in `flush_pipe.drop[]`; `flush_pipe_drain` passes it into `dcache_flush_keep`. All these helpers are private and have no other production callers. The recovery branch at `:5713–5726` therefore cannot execute through the current pipeline.

This mode describes an older ownership policy in which a snapshot stole the body. Current code explicitly retains every snapshot's local body through REPORT. `tests/test_client_memory.py:198–207` manually clears the body and calls `dcache_flush_keep(..., 1)` for the old mode. A test reaching a manufactured private state does not establish a production producer for it.

**Recommendation:** remove the flag, output parameter, per-job array and old recovery branch together, retaining the current failed-PUT re-dirty and successful-PUT image installation behavior. Replace the obsolete test setup with assertions about the current retained-body contract. Do not remove superficially similar recovery branches in `dcache_flush_slot_inner_budgeted`: concurrent cache-chain replacement is a separate condition and has not been proven unreachable.

**Scratch check:** removing the flag/array/recovery branch and fixing every production call site made a temporary `write.c` **25 lines smaller** and passed a complete-file local C syntax check. This deliberately leaves the now-always-zero helper return value in place; API/test cleanup can simplify it further. No modified pipeline was executed.

**Validation:** failed PUT, pipeline-add failure, pressure, overlapping write/flush, exact REPORT ACK and refused teardown. Current `test_client_memory.py` and `test_fold_observation.py` pass at this base.

## S3 — Benchmark path walking has an unselectable permission mode

**Source:** `src/client/ops.c:888–1079`; only caller of `efs_client_lookup` is `src/client/efs_bench.c:722`.

`lookup_walk` is private and called only by `efs_client_lookup`, which always passes `do_x=0` and zero/null credentials. `walk_batch_ancestors` is called only by that walker. The 24-line `lookup_access_ok` helper, the permission branches in both walkers, and their credential parameters support only a mode that no current caller selects. No direct extraction-test consumer was found for this mode.

**Recommendation:** simplify the benchmark walker and its batch helper by removing the disabled permission mode. Preserve pathname validation, owner lookups, local dirty-size overlay and batching/fallback behavior. This does **not** remove FUSE/kernel permission enforcement or the server's query credentials. Do not delete the whole walker: the benchmark uses it to recover inode identities.

**Scratch check:** a temporary complete `ops.c` with this mode and credential plumbing removed was **53 lines smaller** and passed a local C syntax check. No benchmark/network behavior was executed against that copy.

**Validation:** shallow/deep benchmark paths, slash normalization, missing/non-directory components and batch failure fallback. Keep the live LOOKUP_PATH handler and protocol: server code uses it too.

## S4 — Old reserved-field layout behavior has no current source of state

**Source:** `src/client/ops.c:430–447`, `:568–573`, `:654–660`, `:873–875`; `src/client/efs_fuse.c:1381–1382`, `:4922–4927`; metadata row copy sites near `:334–336`, `:369–371`.

The remaining reserved inode fields are copied between local/native inode objects, but no current first-party source generates nonzero values. Server replies are zero-initialized by `stat_to_inode`; authoritative `efs_meta_row` has no such fields. This makes `pull_file_layout`'s 18-line branch and the field-specific allocation/seek/overlay logic likely leftovers from the old metadata engine.

**Recommendation:** retire the field-specific runtime behavior and stale comments after checking downstream/native wire consumers. Retain the public fields as reserved slots until protocol/layout retirement is explicit.

`g_client.ino_namespace` and `ino_counter` also have only two assignments (`ops.c:1612–1613`) and declarations, with no readers. Remove those fields/stores separately. Keep `efs_client_setup_ino_namespace`'s live initialization of `flock_token` from the mount UUID; the function name is misleading, but its whole body is not dead.

## S5 — Two cache patch helpers perform the same work

**Source:** `dcache_patch`, `src/client/write.c:4722–4754` (33 lines); `dcache_patch_sparse`, `:4929–4956` (28 lines).

Both locate the cached chunk under its slot mutex, copy the bytes, re-dirty/pin/account if necessary, add the write range, unlock and invalidate the read cache. Executable differences are limited to the DTRACE label; comments describe different historical failures. One helper can serve both call paths. Preserve the useful concurrency explanation without keeping two implementations.

**Do not blindly merge the loaders above it:** `dcache_load_and_patch` at `:4784–4820` uses the read cache and accepts an unavailable base as an unpublished zero/range image. `dcache_load_sparse` at `:4958–5009` bypasses the read cache and rejects failure to read an existing mapping after two attempts.

An extracted-function probe with a mapped generation, cache miss and injected fetch failure observed regular return **0** with a zero-filled base installed; sparse return **-1** without installation. This is an admission/error-policy difference, **not proof of durable data loss**: the normal flush path (`dcache_steal_dirty_budgeted`, especially `:5616–5674`) fetches a fresh merge base before PUT and refuses a failed fetch. Preserve that safeguard. Consolidating loaders requires an explicit cached/fresh-base policy and tests for admission, reads before flush and retry behavior.

**Validation:** cached patch, mid-flush re-dirty, read-cache invalidation, existing-map fetch failure, holes, append and failed PUT recovery. Start with the common patch primitive; avoid a generic framework for two modes.

## S6 — Perf/strace process management is repeated four times

**Source families:** `src/bench/bench_local.c:45–58`, `:180–216`; `src/client/efs_bench.c:49–100`; `src/client/efs_fuse.c:6799–6922`; `src/server/efsd.c:132–251`.

The mkdir/start/stop function bodies in these families occupy **308 lines**. The two benchmark perf launchers are copies; daemon and FUSE profiling code repeat the longer perf/strace variants and bounded stop sequence. A small shared recorder utility with target PID, output directory, recorder kind and optional filter should replace the repeated subprocess work, leaving thin program-specific startup/shutdown wiring.

Preserve startup after daemonization, target selection, perf frequency, strace expression arguments and bounded TERM/KILL/reap behavior. Do not move unrelated KV/Raft `mkdir_p` functions into this consolidation automatically: their error contracts and durability surroundings differ. The copies all send SIGKILL even after `waitpid(..., WNOHANG)` successfully reaps the child. An extracted helper with stubbed syscalls confirmed the sequence. Once reaped, that PID is no longer owned by this process; a shared implementation should clear/return on successful reap and handle EINTR/ECHILD explicitly. This is another example of a defect duplicated across launchers, not a demonstrated incident involving another process.

**Validation:** recorder exec failure, output-directory failure, target PID selection, recorder exits early, normal shutdown and timeout shutdown. Measure profiler overhead separately from EFS work as before. No profiler runs were launched on a cluster by this review.

## S7 — Address parsing is copied and validation differs

**Locations:** `efs_fuse.c:6722–6734`; `node_cache.c:36–48`; `efs_mgmt.c:22–34`; `efs_query.c:11–23`; `efsd.c:253–265`; stricter benchmark variant `efs_bench.c:109–124`.

Five 13-line parsers are identical. The 16-line benchmark parser adds empty-host and port-range checks. The identical copies use `atoi` followed by narrowing to `uint16_t`, allowing malformed/overflowing values to be silently transformed. The benchmark's `atoi` still does not require complete numeric consumption.

**Recommendation:** one checked parser with bounded host copying and complete decimal port validation, then direct callers or minimal wrappers. Treat changed rejection behavior explicitly. Cover empty host, missing separator, empty/nonnumeric port, trailing characters, negative/zero/greater-than-65535 port, and the IPv6 form the programs actually promise. Do not invent different hostname/address rules for each CLI.

Gross surface is 81 lines. Estimate 45–55 net before overlap; subtract the 13-line query parser if R10 retires the CLI, leaving **32–42** additional in the combined estimate.

## S8 — Byte-order helpers are repeated across formats and simulators

There are **62 short big-endian read/write function bodies totaling 361 physical lines** in KV keys, metadata/session/lock/transaction/tickets, Raft, management, host and simulator files, plus wire framing. Examples: `kv_key.c:4–22`; `meta_apply.c:95–129`; `txn.c:16–39`; `raft_host.c:520–550`; `sim_lock.c:14–37`; `wire.c:118–141`.

A small header of unaligned-safe integer read/write primitives can replace most copies. Keep semantic record encoders local: sharing byte order does not imply sharing a KV key, a Raft log record and a network message. Preserve byte-for-byte output, integer widths and bounds-checking at each caller. Keep little-endian disk helpers separate; they are not included in this 361-line figure. Do not use potentially unaligned integer pointer casts.

**Probe:** extracted all 62 actual helper bodies, renamed them privately and tested 1,000 values each (including zero/all-one values), golden byte order and unaligned buffers under UndefinedBehaviorSanitizer. All passed. This checks primitive equivalence, not every record-format binding.

**Validation:** golden byte encodings, decode round trips and malformed-input gates, including KV snapshot/WAL/segment replay, Raft records and wire tests. The 250–300-line net estimate allows for the replacement header and some wrappers. It concerns source maintenance; inlining/linking already means source duplication is not equivalent to binary bloat.

## S9 and S11 — Namespace rules are implemented in three places

| Implementation | Operations measured | Function-body physical lines |
|---|---|---:|
| Production Raft host | mkdir, rmdir, unlink transaction, link, rename-at | 1,665 |
| Simulator planners | mkdir, rmdir, unlink, link, rename | 829 |
| Direct-KV metadata API | link and rename | 340 |
| Combined domain surface | These selected operations only | **2,834** |

Production examples are `raft_host.c:12176–12465`, `:12473–12813`, `:12823–13023`, `:13930–14129`, `:14145–14777`. Simulator examples are `sim_txn.c:451–649`, `sim_ns.c:390–588`, `:622–1054`. Direct-KV APIs are `meta_apply.c:2497–2616`, `:2726–2945`.

The copies all implement dentry changes, link counts, parent versions/timestamps, replacement and reap obligations, but different wrappers acquire/read/guard/commit state. Simulator comments explicitly mirror production details. This is maintenance risk: a domain rule fixed in one planner can remain wrong in another. It is not proof that a simulator or transaction layer is unnecessary.

The direct-KV link/rename API has **test callers only**, across `test_meta_apply.c`, `test_query_disk.c` and `test_query_catalog.c`. Its exclusive rename helpers `stamp_dir_items` (`:2666–2700`, 35 lines) and `dir_is_under` (`:2705–2724`, 20 lines) bring the bounded retirement surface to **395 gross lines**. `dentry_drop_items` (`:2620–2664`) also serves the live unlink implementation at `:3360`; keep it. Removing both link/rename APIs does not make this shared helper dead. No production caller was found for these APIs. Public-header consumers still require checking.

**First decision:** should tests prepare fixtures through the actual transactional planner rather than maintain a separate direct-KV namespace algorithm? Preserve query fixture expressiveness and assertions. If the APIs are deliberate fixture construction utilities, move them into a test-only boundary and label the reduced contract; moving code does not reduce total LOC.

**Longer-term recommendation:** share pure operation planning and record construction behind a small authoritative-read interface, keeping daemon Raft submission, simulator fault scheduling and independent POSIX expectations separate. Verify each operation's lock/intent/version contract before extraction. Keep an independent oracle so production and simulator do not agree merely because both execute the same bug. Net savings need an implementation prototype; none are claimed here.

## S10 — Serial and pooled writes duplicate the chunk publication sequence

**Source:** `client_write_run`, `src/client/write.c:7289–7406` (118 lines); `write_chunks_no_replicate`, `:7451–7581` (131 lines); pooled worker near `:7153–7283` (131 lines).

Both assemble chunk bytes, hash/encode, choose placement, PUT fragments, update staged chunk mappings and record metadata changes. Serial `efs_client_write` is used by symlink creation and the benchmark; the pooled no-replicate path is used by FUSE writeback. Neither is simply dead.

Differences include admission/flush responsibility, scheduling and deadlines, cached/full-chunk shortcuts, base/observation bookkeeping and size/dirty ordering. Share a chunk job implementation and map-commit routine before considering one executor. A serial mode can run the same job inline without adding worker coordination. Preserve API synchronization semantics and error cleanup. Avoid retaining two independent fixes for each PUT/map accounting bug, but do not change every write into an asynchronous write as an incidental cleanup.

**Validation:** symlink and benchmark writes, partial/full chunk, cache hit/miss, failed PUT, metadata flush/admission, concurrent writes and allocator bounds. No net line estimate is justified without this extraction.

## S12 — Staged features must have a retirement or integration boundary

`publication_writeback.c` (109 physical lines), `publication_ack_owner.c` (15), `put_ticket_owner.c` (110), and `put_upload.c` (57) total **291 lines** before their private RPC, cache-lane and writer-state dependencies. No current FUSE write path selects that complete client flow. The header states that no FUSE caller is enabled (`client_internal.h:522`). Extracted/private tests and the live publication test exercise it.

This chain is not ownerless experimental code: [in-flight](../status/in-flight.md) and [D25 admission routing](../status/d25-admission-routing.md) explicitly describe remaining integration and replay/session/collection gates. Server ticket/publication handlers and persisted records exist. Some lower-level state checks are already on legacy write paths. Whole-chain deletion is not supported by the evidence.

**Recommendation for review:** give the staged chain one activation plan, an accountable owner and a decision checkpoint. Upon activation, retire the replaced legacy ownership path rather than permanently maintaining both models. If abandoned, map shared runtime consumers, persisted commands and recovery formats before deleting the dependency closure. A separate build/test boundary can make staged code easier to audit, but is not itself a LOC reduction.

## S13 — The RAM query backend is a live reference, not an abandoned fallback

**Source:** `src/meta/query_catalog.c` (805 physical lines including the common facade); disk implementation in `src/meta/query_disk.c` and its private headers; host selection at `src/server/raft_host.c:8345–8348`, `:8417–8439`.

The host defaults to the disk backend, but `EFS_QUERY_BACKEND=ram` explicitly selects the RAM implementation. This is documented as a comparison backend in [the commands guide](../using/efs-commands.md). `tests/test_query_disk.c:18–30` compares RAM/disk inode attributes, stats and paged results. The implementations use different data structures and memory scaling, which makes the independent reference useful. RAM also has explicit small-namespace tests and historical measurements; it is not an unreferenced experimental module.

**Recommendation:** decide whether a selectable production RAM mode remains a supported diagnostic requirement. If not, move the reference behind a test/comparison build boundary, preserving differential correctness checks. That reduces production modes, not necessarily source LOC. Do not merge all rollup/path algorithms merely to make the independent oracle share implementation with the code it validates.

One smaller abstraction cost is measurable: the common `efs_query_catalog` wrapper contains the RAM backend's `part[4096]`, `fold[4096]`, `inodes[4096]` and `node_begin[4097]` arrays even for disk catalogs. A type-layout probe on this 64-bit Mac measured **114,832 bytes** for the wrapper, of which **114,692 bytes** are those arrays. Disk construction allocates the full wrapper with `calloc`. Splitting private RAM state from common catalog lifetime/diagnostic state would avoid roughly 112 KiB per disk catalog. This is bounded overhead, not namespace-proportional memory and not a solution to the billion-file scale requirement. It is a boundary cleanup candidate, not justification for deleting the RAM oracle.

## Compatibility layers that are still doing work

- Query request versions 1–4 are accepted by `handler.c:1127–1145`. Version 1 is used by the current mounted stats path; v4 is used by current find/list paging. Removing the uncalled v3 convenience wrapper is different from removing protocol decoding. Node-to-node HELLO identity is not evidence that every application connection negotiates away old formats.
- `efs_session_accept_legacy` is called by current lock, append and metadata paths. The legacy session fence entry point also documents persisted-log use. Retiring a producer does not retire Raft/WAL replay of already persisted commands.
- Generation-zero fragment names remain represented in the store and protocol. A clean new cluster is not proof that old on-disk objects no longer exist.
- The transport/store in-memory implementations are used by tests and simulations. They provide fault/control seams and are not discarded just because production uses sockets/RDMA and disk storage.

For every compatibility removal, record the oldest accepted persisted format and client version, who still produces it, what may remain on disk/in snapshots, and the migration/replay test. Tiny no-op CLI label compatibility is not worth turning into a broad migration project.

## Smaller clones that do not justify a new framework

The bounded scan/delete loop in `src/meta/lock.c:415–439` mirrors `src/meta/session.c:835–859`. The callbacks decode different lock/lease keys and values, and the lease wrapper validates the shard range while the lock wrapper has a different entry contract. A small shared batch-delete primitive is worth considering only if it makes these contracts clearer and actually reduces code after adding its interface. This is not included in the total. Likewise, crash-diagnostic writes and unrelated mkdir helpers have small cloned blocks; avoid replacing a few lines with a larger generic abstraction merely to remove a similarity warning.

## Measurement and validation limits

The accompanying [source inventory](source-inventory-20261011.json) counts tracked `src/` and `include/` C/H files at the pinned base: **107,456 physical lines / 91,774 nonblank lines after lexical comment removal**. C sources alone are 96,620 / 84,272; excluding simulator, benchmark and benchmark-client C gives 84,240 / 72,880. These are explicit scopes, not a recalculation of the earlier 61,476 figure, whose revision and exclusions may differ. The generated `src/client/commands/source.h` contributes 534 physical lines. Third-party dependencies, test sources, result artifacts and docs are outside these counts.

Function scanning and token-root graphs were used only to find candidates. They do not prove C reachability: macros, callbacks, inline functions, optional builds, benches, tests and external APIs require manual checks. Every retained finding above has a caller/producer or contract check in addition to textual similarity. Line estimates are physical source lines and include function-body comments and blank lines; replacement code and overlapping candidates matter.

Completed during this pass:

- Original metadata, client `ops.c` and `write.c` syntax checks: passed on this Mac. Original `test_stage_evict`: passed.
- S2/S3 scratch complete-file syntax checks: passed (25/53 physical lines removed respectively).
- Scratch metadata rollup reduction and the same existing staging test: passed; the staging and 20,000-mutation scratch probes also passed AddressSanitizer/UndefinedBehaviorSanitizer.
- Baseline pending-array accounting probe: confirmed the allocation is omitted from the staging-byte estimate.
- Original versus scratch immediate-count probe for create/link/deferred updates/rename/unlink: passed.
- Original versus scratch 20,000 seeded mutations: matching digest `afdaf51ba528758e` for return codes, ordinary selected attributes, immediate counters and spread decisions. This is an unsharded in-memory probe, not full sharded/FUSE coverage.
- Extracted partial-reallocation failure probe: AddressSanitizer reported the expected double-free during ordinary cleanup.
- Extracted recorder-stop probe with all syscalls stubbed: confirmed a SIGKILL is issued after successful child reap.
- Extracted mapped-base-fetch-failure probe: observed and asserted differing regular/sparse admission behavior.
- Existing `tests/test_client_memory.py`: passed (including read-pressure ownership test).
- Existing `tests/test_fold_observation.py`: passed.
- Query facade and private inode-row type-layout probes: measured sizes on the local 64-bit ABI; hypothetical smaller private rows were not run through the client.
- All 62 extracted big-endian helpers: golden-byte/unaligned checks for 1,000 values each passed with UndefinedBehaviorSanitizer.

No Linux production build or mounted/live cluster gate was run. The temporary probes stub RPC/cache allocation/installation where stated; they cannot establish full mounted behavior. Current branch production source is unchanged. Recheck every finding against the later integration base because other coding branches can add consumers.

## Suggested implementation order

1. Retire the original R1 chain and simple private wrappers; remove S2/S3 constant modes with focused test updates.
2. Implement S1 as one bounded client-cache cleanup, preserving counters, ordinary attributes and native layouts.
3. Consolidate S5/S7, then S6 and S8 with explicit contracts and byte/lifecycle tests.
4. Decide S9's fixture API and public boundary; prototype one namespace planner extraction before projecting larger savings.
5. Keep S10/S12 tied to the actual write/publication integration plan so consolidation retires an old path instead of adding a fourth layer.

The useful goal is fewer independent implementations of each rule. Approximately 1,250–1,520 lines is a defensible planning scale for the bounded candidates and straightforward consolidation; removing several thousand more would require architecture/product decisions that this review does not pretend are already made.
