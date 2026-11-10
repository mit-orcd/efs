# Concern map of `src/client/write.c` (Task T3)

**Merge state (Nov 10):** merged into devel in wave 73; `scripts/check_concern_map.py` is part of `make docs-check` from here on.

**Status (Nov 10, 2026):** source commit devel `862a78945` (the `write.c` of that commit: 9,812 lines). Verdict: **PASS**, docs and one checker only. Remaining failures: none. Active run: none. Next action: the merging session updates the map for functions added to `write.c` after that commit; `make docs-check` names them. Merge state: branch `claude/modest-cray-29bc32`, unmerged.

## Purpose

Task T3 of the [structure review](structure-review-tasks-20261110.md): give an agent with a defect in `src/client/write.c` (K38, K102, K106, K110, K32) a map instead of 9,812 lines. No K row; blocks alpha: no.

## What changed

- New [docs/how-it-works/write-path-map.md](../../how-it-works/write-path-map.md): 20 concerns, each with Does, State and its mutex, Entry points, Functions, Logs, Tests; then a Lock order section. 280 distinct function names (289 definition lines; 9 names are a forward declaration plus the definition), no line numbers.
- New `scripts/check_concern_map.py MAP.md SOURCE.c ...`: exits 1 for defined functions in no concern, mapped names not defined, names in two concerns. No `git`.
- `Makefile`: one line in `docs-check`.
- `src/client/README.md`: pointer line and a "Symptom | Concern" table (13 rows).
- No source or test file changed. `write.c` was not touched.

## Commands and results

| Check | Command | Result |
| --- | --- | --- |
| Checker passes | `python3 scripts/check_concern_map.py docs/how-it-works/write-path-map.md src/client/write.c` | exit 0 |
| Checker fails on a deleted name | same, on a copy with `put_win_wait` removed from its `Functions:` line | exit 1, `defined but in no concern: put_win_wait` |
| Checker fails on an unknown name | same, on a copy with `bogus_fn` added | exit 1, `in the map but not defined: bogus_fn` |
| Map size | `wc -l docs/how-it-works/write-path-map.md` | under 400 lines (see the file) |
| Guide counts | `python3 scripts/check_guide_counts.py` | passes (13 guides) |
| Docs check | `make docs-check` | passes, includes the new line |
| Links, layout, K register | `check_doc_links.py`, `status_layout.py`, `check_k_register.py` | pass |

The copies used for the two failing runs were temporary files outside the repository; the map itself was never edited for the test.

## Question: why can `close()` return EIO after a member stops while a PUT is in flight

Sections I would open, and the `write.c` lines they point at (measured as each function's extent, from its definition to the next one; the file has 9,812):

| Section | Why | Lines |
| --- | --- | --- |
| Fragment PUT send | `put_fragments_parallel_run` retries, re-places and decides the verdict | 918 |
| PUT replies and degraded acknowledgements | `put_drain_verdict` turns the member replies into NO_QUORUM, NET, BUSY or OK (2 of 3) | 335 |
| Node notes and rerouting | the stopped member is marked away and a spare is tried | 169 |
| Write-back error state | where the EIO a later `close()`/`fsync()` reports is recorded | 101 |
| PUT window | a PUT in flight; the flusher waits for it | 105 |
| Flush pipeline (only its `dcache-flush-fail` and `dcache_put_now` functions) | the flush that fails | up to 1,187 for the whole section |

The six sections total 2,815 lines (29 % of the file). Reading only the named functions inside them (`put_fragments_parallel_run`, `put_fragments_parallel_once`, `put_drain_verdict`, `reroute_untried_fragments`, `put_quota_has_spares`, `node_note_away`, `node_note_back`, `node_is_away`, `dcache_put_now_budgeted`, `efs_dcache_flush_ino`, `dcache_flush_ino_pass`, `wb_error_record_add`, `efs_wb_description_sync`, `put_win_wait`) is 1,101 lines (11 %). Without the map the same agent starts from a 9,812-line file.

## Method and limits

- The functions come from the step 1 pattern of the brief. The groups follow the code's own comments and state; a function sits in the concern whose state it touches.
- Mutex ownership comes from the lock calls and comments in the code. Where the code only states an order (`g_unit_mu` before the putid and dcache locks) and no function exercises it, the map says so. The lock-order list holds pairs seen held together; it is not a proof that no other nesting exists.
- Tests are the files that name the concern's functions, most mentions first; `Reclaim` and `Dirty snapshots` have a hand-picked file because no test names their functions.
- The map is not regenerated from the code: it is checked for completeness only. State and Entry text can drift without failing the checker.
- Reproduce: run the three checker commands above on a checkout of devel `862a78945` or later.
