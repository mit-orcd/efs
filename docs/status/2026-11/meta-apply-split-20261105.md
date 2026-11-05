# meta_apply.c split (Task DL, W171 phase 3)

Task DL, Nov 5 2026. Purpose: [W171 phase 3](structure-plan-20261102.md): split `src/meta/meta_apply.c` into `src/meta/meta_apply/` by the 2a/2b recipe, with a compiled-interface test and a measured reading set for one unfamiliar investigation. Verdict: **draft: before-measurement only** (written on devel `0c3e34618` before any code moved).

## Reading set before the split

Scenario (not one of the three fixes Task DB/DD studied, and not investigated before): *"After a publication retire that follows a STALE `why=image-base`, the per-anchor degraded-mark count stays stale."* Method: [reading-set baseline](reading-set-baseline-20261104.md) (lines printed into the agent's context; ranges located by the guide, a graph `explain`, or one grep; "needed" is the reviewer's call after answering the report on paper). Source: devel `0c3e34618`, `meta_apply.c` 11,063 lines, 312 function definitions (159 external, 153 `static`), 9 `static` variables.

Entry: `src/meta/README.md` (50 lines) names `meta_apply.c` and its interface `efs/meta_apply.h` (1,403 lines); it has no symptom map below file level, so the area (W154 degraded marks, W77/K13 publication) was found by grep. `why=image-base` is a raft_host log word; one grep maps it to `EFS_PUB_STALE_BASE_CAS`.

| File | Lines | Category | Needed |
|---|---|---|---|
| `src/meta/README.md` | 1–50 | guide | yes (50) |
| grep `"image-base"\|EFS_PUB_STALE_BASE_CAS` over `src include` | 3 hits | navigation | yes (3) |
| grep `degraded_note(\|degraded_retire(\|PUB_STALE(\|pub_stale_why\|dg_wm_` in `meta_apply.c` | 41 hits | navigation | yes (41) |
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

Understand: **needed 444, opened 669**. Categories (needed): guide 50, navigation 44, interface 15, implementation 319, shared state 15 (dg_wm statics and their rule), outside the module 1. Not opened but on the path: the host's caller of retire (`apply_one_publish`, `raft_host.c` L3160 per `graphify explain efs_meta_apply_publication_retire`) and the rebuild pass that calls `efs_meta_degraded_count_derived`; a real host-side change would open them.

Answer on paper: the report does not reproduce. `PUB_STALE(EFS_PUB_STALE_BASE_CAS)` returns before any batch, so no mark is written and `degraded_note` is not reached; the durable path then writes only a STALE receipt (verdict 2); `efs_meta_apply_publication_retire` deletes the receipt and advances the floor and touches no mark. The count moves only after a written, non-dry batch (`publish_inner_as`, `degraded_clear`, `degraded_mark`, `recovery_mark`).

Validate: `python3 scripts/tests_for.py meta/meta_apply` routes 12 rows; grep over them for the names under question hits only `tests/test_meta_apply.c` (80 hits), which compiles and links the real module through `efs/meta_apply.h`.

| File | Lines | Category | Needed |
|---|---|---|---|
| `tests/budgets.tsv` | the 12 `meta/meta_apply` rows | routing | yes (12) |
| grep over the 12 routed tests | 12 lines | navigation | yes (12) |
| `git grep` over `tests/` for `degraded_count\|publication_retire\|STALE_BASE_CAS\|publish_stale_why` | 6 files | navigation | yes (6) |
| `tests/test_meta_apply.c` | grep hits (24 lines) | navigation | yes (24) |
| `tests/test_meta_apply.c` | 1040–1062 | STALE `BASE_CAS` and its why asserted | yes (23) |
| `tests/test_meta_apply.c` | 5795–5830 | degraded marks retired by a later publish | 21 of 36 |
| `tests/test_publication_recovery.c` | grep `publication_retire` (10 hits) | retire paths; routed `common/publication`, not `meta/meta_apply` | yes (10) |
| `tests/budgets.tsv` | tags of the 5 publication tests (grep) | routing | yes (7) |

Validate: **needed 115, opened 130**. No test pins the count across a STALE publish followed by a retire; the STALE and the count are each asserted, in different cases. The retire tests are routed elsewhere (`common/publication`).

Dead ends (2): the broad graph query (`graphify query "publication retire after STALE why image-base leaves degraded count stale"`: 410 nodes, start nodes `image()`, `degraded()`, `base()`, `retire()` from scripts and tests); `publish_ticketed` / `publish_reject` / `publication_admit` (70 lines opened, not on the durable STALE path).

Files opened: 7 (`README.md`, `meta_apply.h`, `meta_apply.c`, `raft_host.c` one grep line, `budgets.tsv`, `test_meta_apply.c`, `test_publication_recovery.c`). Whole-file upper bound: README 50 + `meta_apply.h` 1,403 + `meta_apply.c` 11,063 = 12,516 lines, plus `test_meta_apply.c` 7,075.

Against the target (one guide, one interface, one implementation area, the routed tests): guide gives the file only; interface is a 15-line slice of a 1,403-line header; implementation is two areas (degraded marks at 4440–4935, publication at 5040–5821) of one 11,063-line file found by grep; the routed tests include the right file but not the retire tests.

### Entry map (degraded-mark count across publish and retire)

| Part | Where (devel `0c3e34618`) |
|---|---|
| Request entry | the host's serialized apply (`apply_one_publish`, `raft_host.c`) calls `efs_meta_apply_publish` (L5781) and `efs_meta_apply_publication_retire` (L5155); marks also come from `efs_meta_apply_degraded_mark` (L4700), `_clear` (L4612), `efs_meta_apply_recovery_mark` (L4775) |
| State it owns | `dg_wm_count`, `dg_wm_pos`, `dg_wm_known` per anchor (L4454–4457), the only process memory on this path; thread-local `pub_stale_why` (L5045) |
| Locks | `dg_wm_mu`, a leaf mutex never held across KV I/O; `pub_stale_why` needs none; the apply calls are serialized by the replicated apply (comment L5136) |
| Persistence boundary | `publish_inner_as`: one `meta_write_batch_as` with chunk, lane, inode, GC, mark DEL/PUT, ticket, receipt, pack candidate (L5693–5703); retire: DEL receipt + PUT floor (L5170–5172); durable STALE: one receipt PUT (L5818); `BASE_CAS` STALE: no write (L5343) |
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
