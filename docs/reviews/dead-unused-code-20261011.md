# Dead and unused code review

**Release update:** [Branch revalidation and release disposition](release-disposition-20261011.md) supersedes removal recommendations for newer packing/query consumers. Counts below describe the original pinned base.

Prepared October 11, 2026 for the later-today review.

The [extended structural review](structural-code-review-20261011.md) examines duplicated implementations, retired modes and larger dependency chains, with scratch validation and separate consolidation estimates.

- Review branch: `devel-code-review`.
- Base: integrated `devel`, commit `48a2719d19fb5a6b0464935c70a967a1f1a86236`.
- Scope: tracked first-party C code, headers, build wiring, tests, scripts, current design/backlog/status documents, and selected symbol history.
- Result: one clearly unreachable legacy implementation, ten uncalled API/helper candidates, one active no-op guard, and one retired CLI requiring a product decision. No implementation was removed or changed.

The strongest immediate cleanup is the old FUSE `.find` parser/runner. Other findings establish **no in-tree caller at this commit**, not that an exported API has no external consumer or could never be useful. No finite search proves future non-use. Existing experiments with tests, measurements, or explicit integration plans are separated below and are not classified as abandoned.

## Review decisions at a glance

| ID | Finding | Confidence / classification | Suggested decision |
|---|---|---|---|
| R1 | Legacy `.find` parser, runner and private output helpers | High: unreachable static implementation | Remove the bounded helper chain |
| R2 | Whole-file KV snapshot importer | High: no in-tree callers; public header | Remove wrapper after API-consumer check |
| R3 | Metadata text-stat renderer | High: no in-tree callers; public header | Remove renderer after API-consumer check |
| R4 | Metadata chunk-slot accessor | High: no in-tree callers; public header | Remove accessor after API-consumer check |
| R5 | Query snapshot v3 client wrapper | High: no in-tree callers; internal header | Remove wrapper, preserve v3 wire compatibility |
| R6 | Old client convenience wrappers, three functions | High: no in-tree callers; internal header | Remove wrappers and correct stale comments |
| R7 | Container mapping-info helper | High: superseded, historically measured | Remove helper, retain engine |
| R8 | RDMA stats accessor and explicit send-quiesce API | High: no in-tree callers; public contract | Decide separately; retain lifetime guarantees |
| R9 | Reserved-name guard that always returns false | High: executable no-op, intentional behavior | Simplify call sites after test-harness adjustments |
| R10 | `efs-query` executable | High: retired operation, still tested compatibility behavior | Keep sentinel or explicitly retire binary |
| K1–K4 | Container engine, W95, D25, test/simulator seams | Evidence of use or explicit remaining work | Retain; do not blanket-delete |

Suggested order: decide R1 first, then internal wrappers R5–R7, then public APIs R2–R4/R8, then R9/R10. Treat any removal as a separate implementation change with its own tests. There is no proposed disk-format change, protocol renumbering, or durability change.

## R1 — Unreachable legacy `.find` implementation

**Locations:** `src/client/efs_fuse.c:419`, `:906–980`, `:993–1021`.

`find_parse_query` and `find_run_query` each appear only at their definitions in tracked source. Both are marked `__attribute__((unused))`. `find_out_add` is called only by that unreachable runner; `struct find_out` and `enum find_match` belong only to this chain. This is dead implementation, not just an infrequently selected runtime option.

The live path is `find_query_ensure` at `:1056`, which parses a v4 command and uses the current snapshot RPC. History corroborates the migration: `527c2fb9` introduced `.find`; `1509e6e1` added the directory-local `.efs` commands and left the old helpers behind.

**Exact removal boundary:** enum at 419; `struct find_out`, `find_out_add`, parser and their comments at 906–980; runner and its comment at 992–1023. Approximately 108 source lines. Preserve the intervening forward declarations and `virtual_access` at 982–990. Preserve current query caches, virtual inode handling, `query_credentials`, the unversioned snapshot RPC, `g_mountpoint`, and other `find_*` helpers. The unversioned RPC remains used by the stats path near `efs_fuse.c:239`; deleting it would be wrong.

**Review question:** Is there any intended restoration of the old writable `.find` interface? No current source caller or exact-symbol plan was found. If not, delete this bounded chain.

**Validation after removal:** Linux client build; query callback-affinity and snapshot-failover tests; mounted `.efs` find/stats commands including permissions and newline-containing names. Existing coverage tests the current path, not these dead functions.

## R2 — Uncalled whole-file KV importer

**Locations:** `src/kv/kv_snap.c:383–433`; `include/efs/kv_snap.h:23–24`.

`efs_kv_group_import_file` has only a definition and declaration, with no tracked callers or tests. It allocates the entire snapshot file, reads it with `pread`, then calls `efs_kv_group_import`. The current server installation path calls `efs_kv_group_import_prepare` at `src/server/raft_host.c:8018` instead.

**Proposed scope:** remove only the file convenience wrapper and its declaration/comment. Keep the buffer importer, prepared installation path, and underlying snapshot format. Tests in `tests/test_kv_lsm.c` exercise the buffer importer; they are not coverage for this wrapper.

**Review question:** Does anyone consume this public header outside this repository? If yes, deprecate or relocate the convenience API; absence of an in-tree caller alone is insufficient to break that contract.

**Validation after removal:** library/server build, KV tests and snapshot-tail tests, plus follower snapshot installation/restart checks. Do not turn this into a redesign of snapshot import.

## R3 — Uncalled metadata statistics formatter

**Locations:** `src/meta/metadata.c:1671–1702`; `include/efs/metadata.h:486`.

`efs_export_format_stats_ex` has no tracked caller or test. It formats ten immediate/tree rollup values as newline-separated text, ignores its export argument, and is not the current `.efs/stats` command renderer.

**Proposed scope:** remove this formatter and its declaration if public API consumers do not require it. For this narrow formatter removal, retain rollup behavior and current server-owned statistics/query implementation. The extended review separately identifies the recursive client-cache aggregation chain as a larger cleanup candidate, preserving immediate counters and wire layout.

**Validation after removal:** library/client/server builds, metadata and query tests, mounted stats oracle. No rollup behavior should change.

## R4 — Uncalled chunk-slot API

**Locations:** `src/meta/metadata.c:3526–3537`; `include/efs/metadata.h:504`.

`efs_export_chunk_slot` has only its definition and declaration. It exposes the internal chunk-index position through `chunk_idx_get`; no in-tree code or test consumes it.

**Proposed scope:** remove this accessor/declaration after checking external consumers. Keep `chunk_idx_get`, chunk lookup/index machinery, neighboring inode-slot APIs and reservation/growth logic. An uncalled accessor does not make its underlying index dead.

**Validation after removal:** library build, metadata tests, client-memory and publication coverage. Confirm no downstream consumer accesses a physical slot through this API.

## R5 — Superseded v3 query wrapper

**Locations:** `src/client/inode_rpc.c:2403–2409`; `src/client/client_internal.h:708`.

`efs_client_rpc_query_snapshot_v3` has no callers or tests in tracked files. The v4 wrappers immediately below it select the appropriate request size for page-version, filter-version (v3), and base-version requests. Therefore removal of this redundant entry point need not remove support for older request layouts.

History: `c50f4b0c` introduced the filtering wrapper. The current FUSE command path uses v4.

**Proposed scope:** remove only the v3 convenience function and prototype. Preserve `struct efs_msg_query_snapshot_v3`, version constants, server decoding, shared RPC logic and v4 size selection. Do not confuse unused client function names with unused wire formats.

**Validation after removal:** client build, query snapshot failover/callback affinity/filter tests, and requests exercising the supported version layouts.

## R6 — Three uncalled client convenience wrappers

| Function | Definition | Declaration | Active underlying path to retain |
|---|---|---|---|
| `efs_rdcache_put` | `src/client/read.c:1100–1103` | `src/client/client_internal.h:475` | `rdcache_put_inner`, `efs_rdcache_put_owned`, worker insertions at `read.c:1300/1304` |
| `efs_client_kick_meta_flush` | `src/client/write.c:2208–2211` | `src/client/client_internal.h:466` | `meta_flush_enqueue`, `efs_client_note_meta_change`, flush workers |
| `efs_dcache_replay_stale` | `src/client/write.c:6449–6452` | `src/client/client_internal.h:365` | `dcache_replay_stale_ex`, budgeted replay, STALE report handling |

Each has only a definition and prototype when exact tokens in executable code are considered. `efs_rdcache_put` also appears in comments at `read.c:1589/1632`, describing a path that now calls the internal insertion function. Those comments should be updated if the wrapper is removed. Existing read-cache tests call `rdcache_put_inner` directly; they do not establish a live caller for the old wrapper.

**Proposed scope:** remove the three wrappers/prototypes and correct the two misleading read-cache comments. Keep their underlying implementation and current callers. History shows these APIs came from previously active read/report/replay work; this is residual API cleanup, not evidence that the features failed.

**Validation after removal:** client build, fence-read/client-memory/report-pressure tests, replay/fan-budget coverage, mounted read-after-write and publication/recovery gates. Source-extraction test harnesses may need boundary updates; that does not establish runtime use.

## R7 — Container helper left after an optimization

**Locations:** `src/server/store_containers.c:263–274`; `src/server/store_containers.h:12`.

`server_container_info` has no current code caller or test. Unlike an abandoned experiment, it has affirmative historical evidence of use: `docs/status/container-profiling-rounds7-8-20261008.md:18` records it in a measured overwrite profile. That report then explains the replacement: container PUT reads one validated mapping for both quota admission and extent retirement instead of reading it twice.

**Proposed scope:** remove the unused mapping-info wrapper and declaration. Retain mapping readers, quota reconciliation, replacement logic and all container-engine code. Do not rewrite historical profiling evidence to make the old symbol disappear.

**Validation after removal:** container build/tests, overwrite quota shrink/growth/failure checks, corruption and replacement mapping-read checks. Historical profiling explains why this helper is leftover, not why the engine should be removed.

## R8 — Unused RDMA public helpers

**Locations:** `efs_rdma_zc_stats` at `src/common/rdma.c:260–266`, prototype `include/efs/rdma.h:78`; `efs_rdma_send_quiesce` at `rdma.c:1621–1635`, prototype `rdma.h:74`.

Neither function has a current in-tree caller or test. The stats accessor reads counters which are still updated by the active two-SGE/copy-fallback send path at `rdma.c:1675/1680`. The sent counter also controls the first-use log; deleting it wholesale would alter behavior.

The quiesce helper is also named in the public send-buffer lifetime contract at `rdma.h:60`. Current destruction destroys the QP directly (`rdma.c:1239` onward); that is not a hidden call to quiesce. Historical documentation mentions quiesce in the destruction story, but current implementation and old prose differ.

**Proposed decision:** the stats accessor can be removed if external consumers do not need it, while preserving the first-use log and making an explicit decision about unused fallback telemetry. Review quiesce separately: either retain/document it as a supported public completion primitive, or remove it with the public contract updated. Do not add a quiesce call simply as a cleanup exercise.

**Validation after any change:** RDMA transport tests and real RDMA send/disconnect/timeout tests, including outstanding zero-copy payload lifetime. TCP-only tests cannot validate this. Keep the zero-copy registration/send subsystem; it has a live call path.

## R9 — Always-false reserved-name guard

**Locations:** `src/client/efs_fuse.c:413–417`; callers at `:4159`, `:4270`, `:4300`, `:4566`, `:4877`.

`name_is_reserved` ignores its argument and returns zero. Five conditional guards therefore never reject an operation. The comment records an intentional policy: legacy names are ordinary user entries, and `.efs` collisions win. This is executable scaffolding with no effect, not an unused symbol.

**Proposed scope:** simplify only those guard branches and remove the no-op helper. Preserve the intended namespace collision behavior. Do not reinstate reserved names. Four source-extraction test harnesses define their own no-op stub (`test_attribute_authority_errno.py`, `test_rename_flags.py`, `test_unlink_authority_errno.py`, `test_create_credentials.py`); inspect them when simplifying the code.

**Validation after removal:** those four tests plus mounted create/link/rename/unlink of legacy special names and `.efs` collisions. Verify normal permission and authority error paths are unchanged.

## R10 — Retired CLI that still has a compatibility purpose

**Locations:** `src/query/efs_query.c` (90 lines), `src/server/handler.c:1148` QUERY_STATS case, `Makefile:147/285/478`, `include/efs/cli_help.h:50–55`.

The executable is still built by `all`, sends QUERY_STATS, and has no successful statistics-return path. Current servers explicitly reply unsupported. The CLI returns 2 for unsupported or legacy placeholder replies and 1 for malformed replies; `--raw` is accepted but produces no successful raw result. Help already describes the endpoint as retired and directs users to a mounted directory's `.efs/stats`.

It is **not unreachable or untested**. `tests/test_query_unavailable.py`, `tests/live/raft_read_freshness.py`, and `tests/live/fragment_integrity.py` depend on its refusal behavior. W80 in `docs/backlog/work-items.md:3280` records the placeholder fix and an explicit implement-or-retire decision; actual logical/per-user CLI integration remains unimplemented. That is evidence of an unresolved product decision, so it does not meet a strict “no possible future use” claim.

**Review choices:**

1. Keep a small compatibility sentinel with accurate help and refusal tests.
2. Retire the binary explicitly: remove its build target/source/help branch and migrate live tests to a protocol-level unsupported check. Preserve the server's safe rejection of old requests and reserve its wire IDs.
3. Sponsor a concrete integration plan with an owner and acceptance gate. Do not leave the binary implying it reports statistics.

**Validation:** unsupported/legacy/malformed reply tests, CLI help/version coverage, and nonempty-export checks proving no fabricated zero totals. Do not remove wire IDs or resurrect placeholder success.

## Code that does not qualify as abandoned

### K1 — Container storage engine: retain

`src/server/store_containers.c` is a selectable daemon backend (`--storage-engine files|containers`), compiled into the server and local bench. It has `tests/test_store_containers.c`, A/B scripts, restart/hash/deletion evidence, and current operator documentation in `docs/operations/storage-engines.md` and `docs/status/container-engine-20261008.md`.

Defaulting to files does not make the container engine dead. Module extraction is a documented proposal in `docs/status/storage-engine-modules-proposal-20261008.md`; implementation had not started in that proposal, so there is no abandoned plugin loader to delete. R7 concerns one residual helper only.

### K2 — W95 indexed-packing model: retain as an experiment

`src/bench/principles_index.c/.h` and the indexed mode of `storage_principles.c` are benchmark/test code, not production storage. The Makefile wires them to benchmark targets and `test_principles_index`/`test_principles_index_fault`; `tests/measure/principles_churn.c` also uses them.

`docs/status/w95-indexed-model-20261008.md` records implemented, accepted finite experimental gates. W95 and `docs/backlog/storage-engine-v2.md` record continuing work. This is evidence of actual use and evaluation even without production adoption. Consider build isolation only if default test/build cost is a concern; do not classify the model as dead.

### K3 — D25 logical resize and typed publication: retain pending integration

`server_raft_host_logical_resize` at `src/server/raft_host.c:11672` has only a definition/prototype in this tree. Nevertheless `docs/status/in-flight.md:52–68` explicitly stages public logical truncate disabled, documents completed internal work, and lists the remaining admission, authority, materialization and acceptance sequence. `docs/status/d25-admission-routing.md` owns the contract.

This is a dormant entry point with a concrete plan, not an ownerless relic. Do not delete publication/history/lane machinery because one public wiring step is disabled. If the team abandons D25 later, first map persistence formats, replay paths, tests and current shared users.

### K4 — Test/simulator and callback seams: retain

`efs_wire_pack/unpack` are exercised in `tests/test_wire.c`; name-index count/revision accessors are exercised in `tests/test_name_index.c`. The simulator and in-memory stores are also consumed by tests. Absence of a daemon caller is not sufficient grounds to remove test infrastructure.

Likewise, FUSE low-level callbacks may be referenced through operation tables rather than direct calls. `ll_inval_entry` is marked unused in a declaration but has actual calls; `find_synthetic_ino` remains used by current virtual inode stat handling. These are explicit false positives rejected by this review.

## Evidence, limits, and reproducible checks

Reference checks used exact symbol tokens across tracked files, followed by inspection of call sites, Makefile wiring, tests and selected history. Broad C scans were triage only: comments, callback tables and source-extraction tests can fool a simple definition/reference counter. Every candidate above was manually checked. No `#if 0` block was found in `src`/`include`; that is not proof that all conditional code is live.

Useful commands, run from the review checkout:

```sh
git rev-parse HEAD
git grep -n -w -e find_parse_query -e find_run_query -e find_out_add -e find_match -- src include tests scripts
git grep -n -w -e efs_kv_group_import_file -e efs_export_format_stats_ex -e efs_export_chunk_slot -- src include tests scripts
git grep -n -w -e efs_client_rpc_query_snapshot_v3 -e efs_rdcache_put -e efs_client_kick_meta_flush -e efs_dcache_replay_stale -- src include tests scripts
git grep -n -w -e server_container_info -e efs_rdma_zc_stats -e efs_rdma_send_quiesce -- src include tests scripts
git log --oneline -S 'find_run_query' -- src/client/efs_fuse.c
git log --oneline -S 'efs_client_rpc_query_snapshot_v3' -- src/client/inode_rpc.c
python3 docs/check-architecture.py
```

Line numbers are for the pinned base. Re-run the searches against the eventual merge base before deletion: other coding branches may gain consumers after this snapshot. The review excludes uncommitted changes, unrelated branches, third-party dependency internals, generated result artifacts and external library consumers. No whole-program linker reachability analysis, Linux production build, or new live-cluster execution was performed for this report. Runtime measurements quoted above are existing repository evidence, not newly repeated measurements.

For the later-today meeting, record a disposition and owner for each row: remove, retain with an explicit contract, or investigate external usage. Start with R1 and the internal wrappers. Require Linux build/test and focused live evidence before merging implementation cleanup. Keep disk compatibility and wire compatibility separate from source/API cleanup.


## Estimated source lines removable

These are physical source lines at the pinned base, including comments/blank lines inside function bodies. They are not a count of executable statements. Header declarations, surrounding comments, removed guard branches, Makefile/help edits and test adjustments are additional; no implementation diff has been made yet.

| Scope | Physical lines | Conditions |
|---|---:|---|
| R1 private dead implementation | 99 | Three function bodies (93), output struct (5), enum (1); about 108 with adjacent comments/spacing |
| R2–R8 ten uncalled function bodies | 148 | Public API and RDMA contract decisions remain |
| R9 always-false helper body | 5 | Caller branches and test-stub changes additional |
| R10 retired CLI source | 90 | Requires explicit retirement and replacement of compatibility tests |
| Total of these bounded source units | **342** | Upper candidate total, not an approved deletion |

The low-risk first step is therefore about **108 lines**, not thousands. Removing all candidates would be roughly **350+ lines** once declarations/guards/build cleanup are included, before replacement tests or compatibility code offset any savings. D25 and the container/indexed-packing implementations are excluded from this estimate.

Body counts for R2–R8: KV importer 51; stats formatter 32; chunk-slot accessor 12; v3 RPC wrapper 7; read-cache put wrapper 4; meta-flush kick wrapper 4; stale-replay wrapper 4; container info 12; RDMA stats 7; RDMA quiesce 15.

## Recommendations for recorded test data

### Inventory at the review base

Tracked `results/` contains **13,434 files / 703,244,433 bytes (670.7 MiB)** of logical file content. Filesystem allocation was approximately 709 MiB; this is not the Git pack size. `results/measure` accounts for 12,818 files / 665.2 MiB, so it is the useful cleanup target. `tests/` itself occupies approximately 4 MiB and primarily contains executable regression sources/scripts, not disposable generated datasets. No extensionless compiled test executable was identified in the tracked test inventory; the extensionless IO-500 `hosts` file is configuration.

Large result formats are uncompressed JSONL (231.5 MiB), uncompressed logs (121.2 MiB), compressed logs (114.0 MiB), strace captures (54.4 MiB), JSON (40.6 MiB), text (28.2 MiB), and `.data` files (26.5 MiB). Extensions are not a deletion rule: `.data` may be profiling evidence, and JSONL may contain the only time-series evidence of a failure.

The existing [results retention policy](../../results/README.md) already permits removal of superseded/debug captures after findings are recorded, while retaining recent gates, current investigations, reference baselines and active citations. When raw evidence is pruned, retain the summary and explicitly identify what was removed. Apply that policy rather than deleting `results/` wholesale.

### D1 — First review target: old container profiling captures

`results/measure/20261008-container-six-rounds` is the largest single run tree: **1,070 files / 72.75 MiB**. Of that, 24 `.strace.gz` files occupy **54.44 MiB**, and 120 `.calls.gz` files occupy **8.76 MiB**. Raw tracing already being compressed means another gzip pass will not solve this.

Recommend reviewing early-round/repeated captures for removal once their extracted findings and final comparison are retained. Keep `matched-summary.json`, source/version/config identities, checksum manifest, extracted reports, verification/restart/deletion results, and a representative before/after capture for each claim that still needs raw inspection. Retain final guard evidence. The profiling status document actively cites this tree, so this is a **selective raw-data reduction**, not deletion of the run directory.

**Potential pool to inspect: 63.2 MiB of trace/call captures; actual approved savings will be smaller after representative evidence is selected.** The follow-up `20261008-container-rounds7-8` is another 10.59 MiB, including 3.24 MiB of call captures and 6.44 MiB of compressed logs. Apply the same selective approach there, preserving its matched validation and corruption/quota evidence.

### D2 — Keep current W93/W97 and durability investigations intact for now

| Run tree under `results/measure/` | Size | Reason to defer pruning |
|---|---:|---|
| `20261009-w97-flow-repeats-v2` | 67.41 MiB | Explicitly cited by current W97 investigation |
| `20261010-w93-closure` | 51.20 MiB | Current W93 closure/failure evidence |
| `20261010-w93-integrated` | 49.89 MiB | Integrated acceptance evidence explicitly retained in status |
| `20261010-w97-latest-nvme` | 41.59 MiB | Recent NVMe/control evidence; establish disposition before deletion |
| `20261009-w93-overlap-hour` | 40.26 MiB | Current overlap/suffix investigation citation |
| `20261009-durability-tracepoints` | 21.97 MiB | Active durability claim and recorder-overhead limitations |

Do not interpret older timestamps or a later “closure” directory as proof that these runs are superseded. Check the investigation owner and acceptance ledger first. For the durability capture, `perf-script.txt.gz` alone is 9.94 MiB and `wal-headers.json` is 9.74 MiB; together they underpin analysis that a compact summary cannot necessarily reproduce.

### D3 — Stop committing every raw run by default

Recommend changing runners in a later implementation change to write new raw captures to a configurable directory outside the source checkout by default, then promote selected evidence into `results/` after review. Continue preserving small canonical run summaries and required acceptance evidence in Git. Existing `COMMIT=1` workflows need a deliberate evidence selection step rather than indiscriminate raw-file staging.

A promoted run should have: source SHA, host/kernel/config/command identity, outcome, measured overhead, summary/oracle results, retained filenames and hashes. For externally archived captures, include the archive location and checksum and verify retrieval before removing the only working copy. Public links are not required; a stable internal evidence location is sufficient.

Keep temporary PID files and completion markers in runtime output rather than canonical evidence when their meaning is already recorded in a summary. Only 57 tracked `.pid`/`finished` markers totaling 351 bytes were found, so this improves clarity, not space.

### D4 — Compress large text only where it preserves the analysis workflow

Uncompressed JSONL/logs total approximately **352.7 MiB**. They are worth considering for gzip or external archival after the investigations settle. Actual compression savings have not been measured. Update scripts that open them, document decompression, and verify analysis results before switching storage format. Preserve summaries/configs in readable form. Compression is preferable to discarding the sole evidence for an unresolved bug.

There are 1,809 duplicate result paths with identical blob content, representing about 1.87 MiB of repeated working-tree bytes. Git already shares identical blobs, so duplicate removal will mostly reduce checkout clutter and will not reclaim that amount from the Git object database. Avoid breaking self-contained run provenance merely to deduplicate small files.

### D5 — Preserve regression fixtures and reference baselines

Retain XFS comparison data, independent correctness oracles, minimal reproductions, nonempty-export fixtures, corruption/restart evidence, and tests tied to current failures. Leave `tests/` source and live-test scripts intact unless the corresponding feature retirement explicitly replaces or removes their acceptance obligation. A test helper used only by tests is not disposable “test data.”

For run-local copied `.c`/`.py` sources, retain snapshots that capture uncommitted instrumentation or an exact historical workload. Only consolidate copies whose equivalence to a retained Git SHA is verified; a current script with the same filename is insufficient evidence.

### D6 — Make any pruning reviewable and avoid history rewriting

Prepare a deletion manifest with path, bytes, reason, active citations, retained replacement evidence and owner decision. Update the affected run summary to record removed raw files and their recoverable pre-cleanup commit. Re-run `python3 docs/check-architecture.py`, check analysis-script inputs separately, and verify retained summaries still support their claims. The documentation link gate does not find every runtime file dependency.

Normal Git deletions reduce fresh checkouts and file-count overhead, but old blobs remain in repository history. Do not promise Git-history size reduction from a pruning commit. History rewriting should be a separate coordinated project because these coding branches are actively shared.

**Recommendation for today's review:** approve a selective manifest for the container profiling traces first; retain current W93/W97/durability evidence; agree on external raw-output defaults for future runs. No test data has been deleted by this report update.
