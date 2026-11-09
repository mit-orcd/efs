# Reading-set measures: understanding, validation, total and necessary expansion (Task EV)

**Verdict: PASS (Nov 9), docs only.** The [reading-set method](reading-set-baseline-20261104.md#measures-task-ev-nov-9)
now defines eight measures (understanding, validation and total needed,
lines opened, irrelevant reading, missed dependencies, correctness,
necessary expansion) and states that "one guide, one interface, one
implementation file and the routed tests" is the target for a
single-owner question, not a criterion for a cross-owner one. The
[structure plan](structure-plan-20261102.md)'s status block reports the
three ownership pilots as understanding, validation and total; each pilot
page's headline table shows them. Understanding needed fell in all three
pilots, validation needed rose in all three, total needed fell in one
(write-back) and rose in two, lines opened fell in all three. No product
code or test changed; no new measurement. No K row; blocks alpha: no.
Status Nov 9: remaining failures none; active run none; next action: the
coordinator merges the branch.

- Purpose: the Nov 9 outside review of the W171 ownership pilots
  ([structure plan](structure-plan-20261102.md) phase 3): the headline
  quoted understanding lines only while validation rose, and "one
  implementation file" read as a universal success criterion although the
  [blind reader](blind-reader-20261109.md) needed the KV apply scope to
  catch a real race.
- Source: devel `9bb50fe79` (wave 67); branch
  `claude/vibrant-ellis-776063`, page edits `41b335d67`, then this page
  and the ledger, then the graph.
- Environment: the Mac; no host, no build, no run.
- Method: every number is copied or subtracted from the pilot pages'
  own reading logs and summary lines; irrelevant reading is lines opened
  minus total needed. Missed dependencies on the blind-reader page come
  from each question's reference read list compared with the reader's log.

## Lines added per file (`git diff --numstat 9bb50fe79 41b335d67`)

| File | Added | Removed | What |
|---|---|---|---|
| `reading-set-baseline-20261104.md` | 28 | 0 | `## Measures (Task EV, Nov 9)`: the eight measures and the single-owner rule |
| `structure-plan-20261102.md` | 13 | 3 | status date and task, the verdict sentence, the ownership bullet's qualifier |
| `writeback-ownership-20261105.md` | 14 | 0 | headline table |
| `taskEB-review-followup-20261108.md` | 14 | 0 | headline table |
| `find-res-ownership-20261108.md` | 14 | 0 | headline table |
| `blind-reader-20261109.md` | 20 | 0 | headline table per question |

## Recomputed headline

Ownership pilots, implementer's own reading, before → after:

| Pilot | Understanding needed | Validation needed | Total needed | Opened (total) | Irrelevant reading | Source files |
|---|---|---|---|---|---|---|
| Write-back pool (Task DM) | 691 → 593 | 306 → 361 | 997 → 954 | 1,380 → 1,172 | 383 → 218 | 7 → 4 |
| Open table (Task EB item 1) | 640 → 543 | 178 → 427 | 818 → 970 | 1,856 → 1,586 | 1,038 → 616 | 9 → 4 |
| `.find` cache (Task EC) | 542 → 428 | 120 → 395 | 662 → 823 | 972 → 840 | 310 → 17 | 7 → 3 |

Missed dependencies, correctness and necessary expansion are not derivable
for the pilots: no reference answer exists for an implementer's reading.

Blind readers (Task EI), one cold reader per question:

| Question | Understanding needed | Validation needed | Total needed | Opened | Irrelevant reading | Missed dependencies | Correctness | Necessary expansion |
|---|---|---|---|---|---|---|---|---|
| 1 unmount report | 754 | 63 | 817 | 888 | 71 (6 dead ends) | none at review; `src/meta/session.c` from the reference list not opened (cited via `session_revoke.c`) | matches | the session revoke and sibling contracts: real |
| 2 GC pending watermark | 952 | 449 | 1,401 | 1,627 | 226 (9 dead ends) | none at review; the reference's degraded test case not opened; the reference missed the KV apply scope | better (twin variant fails at `test_meta_apply.c:5548`) | reaper, `host_apply`, KV apply scope, harnesses: real |

## Checks

`python3 scripts/check_doc_links.py` (0 dangling), `python3
scripts/status_layout.py` (nothing to move), `make docs-check` PASS.

## Not done

- `docs/status/README.md` line 28 (the W171 row) is not edited, by the
  brief; AGENTS.md asks for a README update when a verdict changes, so the
  coordinator owns that row.
- The verdict prose at the top of each pilot page and the plan's ownership
  bullet (`lines needed 691 to 593 ...`) and history pointer still quote
  understanding first; only the headline tables and the status block were
  in scope.
- No owner count for necessary expansion: the logs record files, not
  component tags.
- The pilots' earlier pages (the baseline's three fixes, the DL
  `meta_apply.c` split) were not re-tabled.
