# meta_apply.c split (Task DL, W171 phase 3)

Task DL, Nov 5 2026. Purpose: [W171 phase 3](structure-plan-20261102.md): split `src/meta/meta_apply.c` into `src/meta/meta_apply/` by the 2a/2b recipe, with a compiled-interface test and a measured reading set for one unfamiliar investigation. No K row; blocks alpha: no.

**Verdict: PASS.** The split landed: `meta_apply.c` (11,063 lines) is gone, 20 area files and 13 area headers plus `apply_internal.h` and a component guide replace it, and the code moved without an instruction change (with inlining off, 314 of 331 functions are identical and 17 differ only in end-of-function alignment padding). **Measured result: lines needed to investigate the scenario did not fall (450 before, 461 after); lines opened rose (675 to 852); the whole-file upper bound fell from 12,516 to 1,704 lines. The agent could not stop at one guide, one interface, one implementation file and the routed tests**: the scenario spans two areas (`gc.c` and `publish.c`), so it reads two headers and two files, and neither area has a per-file test route.

- Source: base devel `0c3e34618` (merged `714d05c67`, graph-only), branch `claude/task-dl-meta-apply-split-b963f3`; tested tree `281cabbd1` (all code and test changes; later commits are the evidence scripts, the graph fixture path and docs).
- Commits: `e24a59ce7` before-measurement; `5bf289881` `META_APPLY_SRCS`, the fault-object pattern rule and `csrc.module('meta_apply')` (no code moved); `978c7cbdd` .. `36a8e9de6` one area per commit (20); `865c91fbd` component guide, catalogue routes, doc pointers; `281cabbd1` the four test cases.
- Build: gcc on node9900 (`gcc 11.5.0 20240719, Red Hat 11.5.0-14`), the Makefile's `-O3 -march=native`, TCP, buffered I/O; the private libfuse at `/data1/efs/deps/libfuse` linked with an rpath.

## Files

Each file is a run of the original in original order, except three whole-block moves that keep an area in one file: the chunk-value codec (original L4114–4280) joins `rows.c`, the append cursor and reservation codec (L4951–5041) opens `append.c`, and the pack candidate index (L10943–11063) closes `pack.c`.

| File | Lines | Header | Original lines | Area |
|---|---|---|---|---|
| `rows.c` | 621 | `rows.h` 119 | 37–549 (less the helpers), 4114–4280 | the write checks (`meta_write_*`) and the row codecs (inode, lane, dentry, fence history, lane authority, chunk) |
| `lookup.c` | 402 | `lookup.h` 35 | 550–946 | inode and dentry reads, LOOKUP, RESOLVE, path walks |
| `init.c` | 516 | `init.h` 42 | 947–1459 | inode allocation, opid records, mkfs/init, salt, membership |
| `create.c` | 694 | — | 1460–2148 | CREATE and MKDIR |
| `xattr.c` | 305 | `xattr.h` 22 | 2149–2445 | extended attributes |
| `namespace.c` | 914 | `namespace.h` 25 | 2446–3389 | UNLINK, expiry, LINK, RENAME, SETATTR |
| `readdir.c` | 690 | — | 3390–4034 | READDIR and RMDIR (the only `EFS_FAULTS` site) |
| `reclaim.c` | 83 | — | 4035–4113 | reclaim and orphan reclaim |
| `gc.c` | 676 | `gc.h` 52 | 4281–4950 | GC/reap records, the GC watermark and degraded-mark counts, degraded and recovery marks |
| `publish.c` | 779 | `publish.h` 43 | 5042–5821 | publication receipts, the publish CAS, ticketed and rejected publishes |
| `pub_stage.c` | 536 | `pub_stage.h` 26 | 5822–6349 | staged publication, transaction resolve and retire |
| `lanes.c` | 701 | `lanes.h` 28 | 6350–7043 | lane activation, content fences, resize, lane bootstrap, writer views |
| `mtime.c` | 140 | — | 7044–7176 | mtime capture and execute |
| `sweep.c` | 679 | `sweep.h` 33 | 7185–7865 | lane sweep, reap, GC ACK |
| `attr.c` | 383 | `attr.h` 31 | 7866–8239 | GETATTR, epoch fence, UTIMENS |
| `append.c` | 1,443 | `append.h` 33 | 4951–5041, 8240–9586 | APPEND, lazy truncate, session drain |
| `chunk.c` | 514 | — | 9587–10092 | chunk rows and views, inode authority, chunk sweep |
| `check.c` | 143 | — | 10093–10232 | the store check |
| `pack.c` | 496 | — | 10233–10599, 10943–11063 | small-file packing |
| `tsweep.c` | 353 | `tsweep.h` 20 | 10600–10942 | truncate sweep, sweep done, retire |
| `apply_internal.h` | 77 | — | 1–36, 153–180 | include block, value sizes, five byte helpers |

11,068 `.c` lines and 586 header lines (11,654 in all, against 11,063). The largest file is `append.c` (1,443); `src/meta/meta_apply/README.md` (104 lines) maps symptom to file, header and tests.

## What changed besides the move

- **`static`:** 61 static functions used by another file lost `static` and are declared in their owner's header (56 become module-internal global symbols; no name collides in `libefs.a`, checked with `nm`). The five byte helpers (`be16`, `rd16`, `be64`, `rd64`, `max_u64`) moved to `apply_internal.h` as `static inline`, the one keyword added, so every file keeps them inlined as before and they are not link symbols. No static variable crosses a file: the `gc_wm_*` / `dg_wm_*` counts stay private to `gc.c`, `pub_stale_why` to `publish.c`.
- **Types:** `struct lane_rec`, `struct append_cur`, `struct publish_mode` and `struct packed_retirement` moved into their owner's header verbatim.
- **Prototypes:** two redundant forward prototypes (`epoch_lane_upd`, `fence_lane_bits` at original L7178–7184, both defined before every use) were dropped; `fence_lane_bits` is declared in `attr.h`. The `pub_stage_owner` forward prototype became its declaration in `pub_stage.h`, which `rows.c` includes (the write check reaches up to the staged-publication owner; the guide names it). One intermediate commit (`append.c`) carried two transient declarations for `attr.c` functions, removed by the next commit.
- **Mechanics:** `Makefile` `META_APPLY_SRCS` (twenty files) in `COMMON_SRCS`, the module objects depend on `$(wildcard src/meta/meta_apply/*.h)`, the fault daemon's `FAULT_META_OBJS` follows the list through a `tests/faults/meta/%.o` pattern rule. `tests/csrc.py` `MODULES['meta_apply']` (override `EFS_META_SOURCE` kept); `test_readdir_migration_race.py` and `test_retained_create_authority.py` read through it, assertions unchanged. `tests/budgets.tsv`: eight rows route to one file (`meta/meta_apply.append` ×4, `.pub_stage`, `.readdir`, `.mtime`, `.lanes`); `test_meta_apply`, `test_session_owned_capture`, `test_namespace_session` and `test_meta_cmd_namespace.py` span files and keep the bare tag. `src/meta/README.md`, `developing.md`, the `settings.md` source column, three historical links and the graph's W125 navigation fixture point at the new files.
- **Check:** `python3 scripts/evidence/dl/verify_split.py` compares the normalised line multiset of the base file with the directory: the 65 lines only in the original are all `static` lines (or the dropped prototype's continuation); the lines only in the split are guards, includes, comments, declarations and the five `static inline` helpers. Each of the 20 carve states compiled warning-free on macOS (normal and `EFS_FAULTS=1`) with the same external undefined symbols as the unsplit object before it was committed.
- **Counts:** the brief's "91 function definitions, 57 static, 34 exported" do not match the file: it held 312 function definitions, 159 external (all declared in a public header) and 153 `static`.

## Instruction stream

`scripts/evidence/dl/instruction_stream.sh BEFORE AFTER OUT` (comparing with `scripts/evidence/dl/objcmp.py`) on node9900, per function, `objdump -d -r --no-show-raw-insn` normalised (addresses dropped, jump targets by symbol):

- **As built (`-O3`):** 249 functions before, 266 after: 92 identical, 144 differ, 13 only before, 30 only after. Every difference is an inlining or cloning decision: 68 functions are shorter (a callee that became external is now called instead of inlined, e.g. `create_file_batch`, `activate_lanes`), 52 longer (GCC's inline budget is per translation unit, so a smaller file inlines more, e.g. `alloc_next_free`, `dir_lane_stamp`), 21 differ only in call targets, 3 in register allocation. The 13 "only before" are clones and inlined bodies (`meta_write_batch_as.constprop.*`, `lookup_at`, `pack_chunk`); the 30 "only after" are the newly external functions now emitted out of line (`meta_write_batch`, `degraded_note`, `lane_adopt`, …).
- **Inlining and IPA off** (both trees' module objects rebuilt from the Makefile's own compile command plus `-fno-inline -fno-inline-small-functions -fno-inline-functions-called-once -fno-partial-inlining -fno-ipa-{cp,sra,icf,pure-const,reference,modref,vrp,bit-cp,ra,profile,stack-alignment} -fno-optimize-sibling-calls`; relocations folded into their calls): **331 functions on each side, 314 identical, 17 differ only by alignment padding at the end** (`nop` / `xchg %ax,%ax`; the functions sit at new offsets). No instruction changed; the as-built differences are the compiler's inlining choices.
- One build warning disappeared: GCC's `-Wmaybe-uninitialized` for `d.type` in the old `meta_apply.c` (a flow-analysis false positive that depends on inlining). Before 5 warnings, after 4, no new one.

## Tests through the interface

`tests/test_meta_apply.c` already compiled the real module through `efs/meta_apply.h` and called functions of 17 of the 20 areas. Four cases were added where an area had none (through `efs/meta_apply.h` and `efs/pack_mapping.h`, linked against `libefs.a`):

- `test_publication_receipt_stale_retire` (`publish.c`, `gc.c`): a durable receipt across a `BASE_CAS` STALE and both retires; the STALE's verdict is durable, the older receipt retires first, and the degraded mark and its count are unchanged throughout. This pins the before-measurement's scenario, which no test covered.
- `test_pub_stage_record` (`pub_stage.c`): stage record round trip, short and unknown-version records `EFS_ERR_PROTO`, encode refusals (durable receipt, no ticket, foreign session), empty-store peek and pin.
- `test_mtime_capture_execute` (`mtime.c`): capture refusals, a changed plan prepares nothing, the first prepare refusal decides and resolves ABORT, otherwise COMMIT.
- `test_pack_index_cursor` (`pack.c`): backfill cursor round trip, a refused INDEX writes nothing, a foreign group is refused, cancelling an unknown pack object is `EFS_ERR_NOT_FOUND`.

`./tests/test_meta_apply` passes on node9900 (1.0 s in the gate, budget 5 s); `python3 scripts/tests_for.py meta/meta_apply --run` (12 rows) and the two splice tests pass.

## Gate and TSan

Host node9900 (192 cpus, Linux, gcc 11.5.0, private libfuse 3 with rpath, TCP, buffered I/O; idle at start, load 1.4), tree `281cabbd1` from `git archive`, `TMPDIR=/tmp/efs-taskDL-tmp/gate`, in a detached screen:

    EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh
    TOTAL 589 FAIL 0 NOTRUN 4 STATUS INCOMPLETE (641s) BUILD 0s

- The four NOTRUN rows are this host's environment: `test_cluster_stop.py` (no `~/git/cluster`), `test_rdma_memcheck.py` and `test_leak_gate.py` real-memcheck fixtures (no valgrind), `test_qualify_rdma_live.py` (no RDMA hardware; its other checks PASS). No over-budget row; INCOMPLETE is the gate's status for NOTRUN with 0 FAIL (exit 3).
- `python3 tests/test_make_test_recipe.py`: PASS. `tests/budgets.tsv` changed only in the subsystem column of eight rows.
- `make all`: 5 warnings before, 4 after, none new (above). Link: no duplicate global in `libefs.a`.
- TSan: `EFS_SAN_LOGDIR=... make tsan-check`: **VERDICT: PASS (22 run, 0 failed, 562s)**. The ten routed meta C tests (`test_meta_apply`, `test_append_transaction`, `test_pub_stage`, `test_mtime_barrier`, `test_session_owned_capture`, `test_append_receipt`, `test_append_cleanup`, `test_append_interleave`, `test_lane_bootstrap_recovery`, `test_namespace_session`) run from the TSan tree with the gate's `TSAN_OPTIONS`: all rc 0, 0 ThreadSanitizer reports. The two routed Python splice tests compile their own unsanitized snippet and were run in the normal tree (PASS).
- Checks: `python3 scripts/check_include_boundary.py` (PASS, 22 includes, 8 pairs: the module headers include only each other, inside one directory), `make docs-check`, `check_doc_links.py` (0 dangling), `gen_settings.py --check`, `test_env_inventory.py`, `tests/csrc.py`, `test_graph_community_labels.py`, graph rebuild and `test_code_graph.py` pass.

## Reading set before the split

Written and committed (`e24a59ce7`) before any code moved. Two tallies are corrected here from that commit: the in-file grep returns 47 lines, not 41, and the test-file grep 22, not 24 (recounted from the same commands); the totals below carry the correction.

Scenario (not one of the three fixes Task DB/DD studied, and not investigated before): *"After a publication retire that follows a STALE `why=image-base`, the per-anchor degraded-mark count stays stale."* Method: [reading-set baseline](reading-set-baseline-20261104.md) (lines printed into the agent's context; ranges located by the guide, a graph `explain`, or one grep; "needed" is the reviewer's call after answering the report on paper). Source: devel `0c3e34618`, `meta_apply.c` 11,063 lines, 312 function definitions (159 external, 153 `static`), 9 `static` variables.

Entry: `src/meta/README.md` (50 lines) names `meta_apply.c` and its interface `efs/meta_apply.h` (1,403 lines); it has no symptom map below file level, so the area (W154 degraded marks, W77/K13 publication) was found by grep. `why=image-base` is a raft_host log word; one grep maps it to `EFS_PUB_STALE_BASE_CAS`.

| File | Lines | Category | Needed |
|---|---|---|---|
| `src/meta/README.md` | 1–50 | guide | yes (50) |
| grep `"image-base"\|EFS_PUB_STALE_BASE_CAS` over `src include` | 3 hits | navigation | yes (3) |
| grep `degraded_note(\|degraded_retire(\|PUB_STALE(\|pub_stale_why\|dg_wm_` in `meta_apply.c` | 47 hits | navigation | yes (47) |
| `include/efs/meta_apply.h` | grep `degraded` (22 hits: 1037, 1098–1160, 1248–1253) | interface | 15 of 22 |
| `src/meta/meta_apply.c` | 4440–4460 | shared state: `dg_wm_*` rules comment and statics | 15 of 21 (4440–4446 is the GC tail) |
| `src/meta/meta_apply.c` | 4459–4522 | implementation: `degraded_note`, `efs_meta_degraded_count_{peek,derived,invalidate,zero_if}` | yes (64) |
| `src/meta/meta_apply.c` | 4680–4700 | `efs_meta_apply_degraded_clear` tail: note −1 after the batch | 8 of 21 |
| `src/meta/meta_apply.c` | 4760–4772 | `efs_meta_apply_degraded_mark`: note +1 | 4 of 13 |
| `src/meta/meta_apply.c` | 4800–4814 | `efs_meta_apply_recovery_mark`: note +1 | 4 of 15 |
| `src/meta/meta_apply.c` | 4895–4935 | implementation: `degraded_retire` (skips the lookup on a known-zero count) | 33 of 41 |
| `src/meta/meta_apply.c` | 5040–5060 | `pub_stale_why`, `PUB_STALE` | 14 of 21 |
| `src/meta/meta_apply.c` | 5128–5205 | session accept, admit, **retire**, receipt verdicts, `publish_mode` | 45 of 78 (admit, accept, mode: no) |
| `src/meta/meta_apply.c` | 5206–5240 | `publish_inner_as` locals (`dg_anchor`, `dg_added`, `dg_removed`) | yes (35) |
| `src/meta/meta_apply.c` | 5330–5350 | the `BASE_CAS` STALE returns before any batch | yes (21) |
| `src/meta/meta_apply.c` | 5600–5724 | superseded marks retired, new mark, batch, `degraded_note` after a written batch | 50 of 125 (ticket, candidate, pack mapping: no) |
| `src/meta/meta_apply.c` | 5725–5821 | `publish_ticketed`, `publish_reject`, `efs_meta_apply_publish` (STALE receipt, verdict 2) | 41 of 97 (ticketed, reject: no) |
| `src/server/raft_host.c` | 23646 (grep hit) | the host's `image-base` log line | yes (1) |

Understand: **needed 450, opened 675**. Categories (needed): guide 50, navigation 50, interface 15, implementation 319, shared state 15 (dg_wm statics and their rule), outside the module 1. Not opened but on the path: the host's caller of retire (`apply_one_publish`, `raft_host.c` L3160 per `graphify explain efs_meta_apply_publication_retire`) and the rebuild pass that calls `efs_meta_degraded_count_derived`; a real host-side change would open them.

Answer on paper: the report does not reproduce. `PUB_STALE(EFS_PUB_STALE_BASE_CAS)` returns before any batch, so no mark is written and `degraded_note` is not reached; the durable path then writes only a STALE receipt (verdict 2); `efs_meta_apply_publication_retire` deletes the receipt and advances the floor and touches no mark. The count moves only after a written, non-dry batch (`publish_inner_as`, `degraded_clear`, `degraded_mark`, `recovery_mark`).

Validate: `python3 scripts/tests_for.py meta/meta_apply` routes 12 rows; grep over them for the names under question hits only `tests/test_meta_apply.c` (80 hits), which compiles and links the real module through `efs/meta_apply.h`.

| File | Lines | Category | Needed |
|---|---|---|---|
| `tests/budgets.tsv` | the 12 `meta/meta_apply` rows | routing | yes (12) |
| grep over the 12 routed tests | 12 lines | navigation | yes (12) |
| `git grep` over `tests/` for `degraded_count\|publication_retire\|STALE_BASE_CAS\|publish_stale_why` | 6 files | navigation | yes (6) |
| `tests/test_meta_apply.c` | grep hits (22 lines) | navigation | yes (22) |
| `tests/test_meta_apply.c` | 1040–1062 | STALE `BASE_CAS` and its why asserted | yes (23) |
| `tests/test_meta_apply.c` | 5795–5830 | degraded marks retired by a later publish | 21 of 36 |
| `tests/test_publication_recovery.c` | grep `publication_retire` (10 hits) | retire paths; routed `common/publication`, not `meta/meta_apply` | yes (10) |
| `tests/budgets.tsv` | tags of the 5 publication tests (grep) | routing | yes (7) |

Validate: **needed 113, opened 128**. No test pins the count across a STALE publish followed by a retire; the STALE and the count are each asserted, in different cases. The retire tests are routed elsewhere (`common/publication`).

Dead ends (2): the broad graph query (`graphify query "publication retire after STALE why image-base leaves degraded count stale"`: 410 nodes, start nodes `image()`, `degraded()`, `base()`, `retire()` from scripts and tests); `publish_ticketed` / `publish_reject` / `publication_admit` (70 lines opened, not on the durable STALE path).

Files opened: 7 (`README.md`, `meta_apply.h`, `meta_apply.c`, `raft_host.c` one grep line, `budgets.tsv`, `test_meta_apply.c`, `test_publication_recovery.c`). Whole-file upper bound: README 50 + `meta_apply.h` 1,403 + `meta_apply.c` 11,063 = 12,516 lines, plus `test_meta_apply.c` 7,075.

Against the target (one guide, one interface, one implementation area, the routed tests): guide gives the file only; interface is a 15-line slice of a 1,403-line header; implementation is two areas (degraded marks at 4440–4935, publication at 5040–5821) of one 11,063-line file found by grep; the routed tests include the right file but not the retire tests.

### Entry map (degraded-mark count across publish and retire)

| Part | Where (devel `0c3e34618`) |
|---|---|
| Request entry | the host's serialized apply (`apply_one_publish`, `raft_host.c`) calls `efs_meta_apply_publish` (L5781) and `efs_meta_apply_publication_retire` (L5155); marks also come from `efs_meta_apply_degraded_mark` (L4700), `_clear` (L4612), `efs_meta_apply_recovery_mark` (L4775) |
| State it owns | `dg_wm_count`, `dg_wm_pos`, `dg_wm_known` per anchor (L4454–4457), the only process memory on this path; thread-local `pub_stale_why` (L5045) |
| Locks | `dg_wm_mu`, a leaf mutex never held across KV I/O; `pub_stale_why` needs none; the apply calls are serialized by the replicated apply (comment L5136) |
| Persistence boundary | `publish_inner_as`: one `meta_write_batch_as` with chunk, lane, inode, GC, mark DEL/PUT, ticket, receipt, pack candidate (the two calls at L5701, L5705); retire: DEL receipt + PUT floor (L5170–5172); durable STALE: verdict 2 and one receipt batch (L5806–5816); `BASE_CAS` STALE: no write (L5343–5344) |
| Completion | `degraded_note(anchor, added − removed)` after a written non-dry batch (L5709–5710); the count is derived by `efs_meta_degraded_count_derived` from one prefix count (host rebuild pass) and invalidated on snapshot import |
| Tests | `test_meta_apply.c`: `test_degraded_marks` (L5593), `test_degraded_mark_after_publish` (L5734), `test_recovery_marks` (L5842), stale why (L1040–1062); retire: `test_publication_recovery.c` (`common/publication`) |

Reproduce (any checkout at `0c3e34618`, no build):

```
graphify query "publication retire after STALE why image-base leaves degraded count stale"
graphify explain efs_meta_apply_publication_retire
git grep -n '"image-base"\|EFS_PUB_STALE_BASE_CAS' -- src include
grep -n "degraded_note(\|degraded_retire(\|PUB_STALE(\|pub_stale_why\|dg_wm_" src/meta/meta_apply.c
python3 scripts/tests_for.py meta/meta_apply
git grep -l "degraded_count\|publication_retire\|STALE_BASE_CAS\|publish_stale_why" -- tests
```

## Reading set after the split

Same scenario, same method, on the split tree (`281cabbd1`, graph rebuilt). The code moved verbatim, so every implementation range of the before-table maps one to one (`gc.c` = original − 4273; `publish.c` = original − 5033, and − 5039 after the moved `struct publish_mode`).

Entry: `src/meta/README.md` routes to `meta_apply/README.md`, whose table names `gc.c` ("degraded marks (W154) and their count") and `publish.c` ("receipts (admit, result, retire), the publish CAS and its STALE reasons") right first time, and says to read each header first. Both rows say "bare tag": no per-file test route.

| File | Lines | Category | Needed |
|---|---|---|---|
| `src/meta/README.md` | 1–50 | guide (routes to the module guide) | 1 of 50 |
| `src/meta/meta_apply/README.md` | 1–104 | guide: intro, table header and the two rows, bare-tag note, Contract | 45 of 104 |
| grep `"image-base"\|EFS_PUB_STALE_BASE_CAS` | 3 hits | navigation | yes (3) |
| grep (same pattern) in `gc.c`, `publish.c` | 33 + 14 hits | navigation | yes (47) |
| `src/meta/meta_apply/gc.h` | 1–52 | interface: the counts' rules and lock, `degraded_key/retire/note`, declarations | 24 of 52 |
| `src/meta/meta_apply/publish.h` | 1–43 | interface: `pub_stale_why`, `publish_inner_as` ("every refusal returns before the batch; the count follows only a written, non-dry batch") | 17 of 43 |
| `src/meta/meta_apply/gc.c` | 167–187 | shared state: the rule comment (now restated in `gc.h`) and the statics | 4 of 21 |
| `gc.c`, `publish.c` | the before-table's eleven implementation ranges | implementation | 319 of 531 (unchanged) |
| `src/server/raft_host.c` | 23646 (grep hit) | outside the module | yes (1) |

Understand: **needed 461, opened 852** (before 450 / 675). Categories (needed): guide 46 (before 50), navigation 50 (50), interface 41 (15), shared state 4 (15), implementation 319 (319), outside 1 (1). `efs/meta_apply.h` is no longer opened: the area headers carry the contract.

Validate: the routing is the bare tag, so the split alone changes nothing (113 needed / 128 opened, as before). With the case this task added, the agent finds the scenario pinned directly:

| File | Lines | Category | Needed |
|---|---|---|---|
| `tests/budgets.tsv` | the 12 `meta/meta_apply` rows (`.gc` and `.publish` route nothing) | routing | yes (12) |
| grep over the 12 routed tests | 12 lines | navigation | yes (12) |
| `git grep` over `tests/` (same pattern) | 6 files | navigation | yes (6) |
| `tests/test_meta_apply.c` | grep hits (31 lines) | navigation | yes (31) |
| `tests/test_meta_apply.c` | 6978–7049 | `test_publication_receipt_stale_retire` and its helper | 69 of 72 |

Validate with the new case: **needed 130, opened 133**; the older STALE and degraded slices and the `common/publication` retire tests are no longer needed for this question.

Graph (rebuilt): the broad query is the same dead end (410 nodes, start nodes `image()`, `degraded()`, `base()`, `retire()`); `graphify explain degraded_note` lists the same six edges, now naming `gc.c` and `publish.c` (the cross-file call from `publish_inner_as` is `INFERRED`, where the single file had it `EXTRACTED`); `graphify explain efs_meta_apply_publication_retire` points at `publish.c` L122.

Dead ends (2, as before): the broad graph query; `publish_ticketed` / `publish_reject` / `publication_admit` (70 lines opened in `publish.c`). Files opened: 9 (`src/meta/README.md`, `meta_apply/README.md`, `gc.h`, `publish.h`, `gc.c`, `publish.c`, the `raft_host.c` grep line, `budgets.tsv`, `test_meta_apply.c`). Whole-file upper bound: 50 + 104 + 52 + 43 + 676 + 779 = **1,704 lines** (before 12,516).

### Before and after

| | Before (`0c3e34618`) | After (`281cabbd1`) |
|---|---|---|
| Guide gave the right file first time | file only (11,063 lines); area by grep | yes, both areas, from the symptom table |
| Lines to understand, needed / opened | 450 / 675 | 461 / 852 |
| Lines to validate, needed / opened | 113 / 128 | 113 / 128 split alone; 130 / 133 with the new case |
| Files opened | 7 | 9 |
| Dead ends | 2 | 2 |
| Whole-file upper bound | 12,516 | 1,704 |
| One guide | yes (no symptom map) | two hops (`src/meta/README.md`, then the module guide) |
| One interface | a 15-line slice of a 1,403-line header | no: two area headers (41 needed lines) |
| One implementation file | no: two areas of one 11,063-line file | no: `gc.c` and `publish.c` |
| Routed tests | bare tag, retire tests elsewhere | bare tag; the new case pins the scenario |

Plainly: **lines needed did not fall** (+11); the reading moved from a 1,403-line public header slice and grep inside one 11,063-line file to two area headers and two files of 676 and 779 lines, and the headers add 26 needed lines that the old file did not have. What fell is what an agent risks opening: the whole-file bound by a factor of 7, and the public header drops out of the reading set. The target (one guide, one interface, one implementation area, the routed tests) is not met for a cross-area question, as for the FUSE pilot's fixes 1 and 3.

### Entry map after the split

| Part | Where (`281cabbd1`) |
|---|---|
| Request entry | `efs_meta_apply_publish` (`publish.c` L742), `efs_meta_apply_publication_retire` (`publish.c` L122); marks: `efs_meta_apply_degraded_mark`, `_clear`, `efs_meta_apply_recovery_mark` (`gc.c`) |
| State it owns | `dg_wm_*` (`gc.c` L181–184), stated in `gc.h`; `pub_stale_why` (`publish.c` L12), stated in `publish.h` |
| Locks | `dg_wm_mu`, leaf (`gc.h` Locks); otherwise the serialized apply (README Contract) |
| Persistence boundary | `publish_inner_as` one `meta_write_batch_as` (`publish.c` L662, L666); retire L137–139; durable STALE receipt L767–777; `BASE_CAS` return L305 |
| Completion | `degraded_note` after a written non-dry batch (`publish.c` L670–671; `gc.h` and `publish.h` say so) |
| Tests | `test_meta_apply.c` `test_publication_receipt_stale_retire` (L7010) plus the existing degraded cases |

## Limits

- The reading set is one scenario read on paper by the agent that did the split, which knows the code better than an unfamiliar agent would; the before table was committed first to limit that bias, and both tables count the same commands.
- The scenario spans two areas by nature; a single-area question (for example a READDIR fault, `readdir.c` with its own route) would read one header and one file. Only one scenario was measured, as the brief asked.
- The two splice tests still read the module as text through `tests/csrc.py`; nothing else splices it.
- The gate host is node9900 only; macOS cannot link `libefs.a` (`fopencookie`), so the intermediate carve commits were checked by compiling the module objects and their symbols, and only the final tree was built, tested and gated.
- `tests/faults/meta/` objects are not removed by `make clean` (as before the split).
- `scripts/graph-community-labels.py` still names `src/meta/meta_apply.c` (as it names `efs_fuse.c`); the topic falls back to the file stem.

## Reproduction

```
git checkout 281cabbd1
python3 scripts/evidence/dl/verify_split.py 0c3e34618          # line-multiset check
git archive --format=tar.gz 0c3e34618 | tar -xzf - -C BEFORE     # on the build host
git archive --format=tar.gz 281cabbd1 | tar -xzf - -C AFTER
(cd BEFORE && make -j32 all) ; (cd AFTER && make -j32 all)
bash AFTER/scripts/evidence/dl/instruction_stream.sh BEFORE AFTER OUT
cd AFTER && EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh && python3 tests/test_make_test_recipe.py
EFS_SAN_LOGDIR=L make tsan-check     # then the meta binaries from L/build under the same TSAN_OPTIONS
python3 scripts/tests_for.py meta/meta_apply --run
```

On node9900 export `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse`, `PKG_CONFIG_PATH` and `LD_LIBRARY_PATH` to its `lib64`, and build with `FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,$EFS_FUSE_PREFIX/lib64"` (screen drops `LD_LIBRARY_PATH`). Reading-set commands: the before-section's block plus `graphify explain degraded_note` on the rebuilt graph.

## Raw data

Outside Git, on node9900: `~/efs-artifacts/taskDL-20261105/` (`stage1.txt`, `stage2.txt`, `gate-summary.txt`, `gate-timings.tsv`, `gate-progress.txt`, `gate-build.log`, `recipe.log`, `tsan.log`, `tsan-meta.txt`, `test_meta_apply.log`, `routed-meta.log`, the warning lists and diff, `objcmp.txt` and `objcmp-noinline.txt` with their reproductions by the committed script, `noinline-flags.txt`, `objcmp-diffs.tar.gz` with every per-function diff), `SHA256SUMS` sha256 `90a744cdfd885dce689b7d09bb6b7c2892989f47483224ad0a19aa6fa5f8d774`. The scratch trees `/tmp/efs-taskDL*` were removed.
