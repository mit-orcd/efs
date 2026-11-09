# One current-status block per active W171 page (Task EH)

**Verdict: PASS (Nov 9), docs only; no source, test or host. No K row;
blocks alpha: no.** Each of the six W171 pages below now opens with one
block that states source, verdict, remaining failures, active run, next
action and merge state, and the W171 row of the [status index](../README.md)
says "merged" with one clause per wave 65 task. Branch
`claude/task-eh-status-blocks-248e6a` on devel `33cd0a839` (wave 65), not
yet merged.

- Purpose: brief EH of the [Nov 9 review](review-feedback-nov9-20261109.md)
  (item 5): active pages kept historical status above the current one (the
  plan opened with "PROPOSED ... no code changed", merged task pages said
  "Not merged", the W171 row said "Task EB (Nov 8, unmerged)"). W171,
  no release gate.
- Source: devel `33cd0a839`; this branch's docs commit and its graph
  rebuild. Mac checkout only; no build.

## Lines moved and replaced

Line numbers are those of devel `33cd0a839` (before) and of this commit
(after).

| File | Replaced | Moved | Other |
| --- | --- | --- | --- |
| [structure-plan-20261102.md](structure-plan-20261102.md) | L3–4 (the "Status: PROPOSED Nov 2" line and the page description) by the status block, after L3–21, the description now L23–25 | L3 (old opening line), L6–19 (the quoted Nov 8 Task EB and Nov 9 Task EC pointers) and L25 ("Current state (Nov 2)") to a new `## History` section, after L258–285, text unchanged | L212 "unmerged" in the Nov 6 write-back bullet became "merged in wave 60" (after L217), since the brief's grep allows the word only in History. The block carries the merge rule line. The old quote had no blank line before "Goal, …", so the goal paragraph rendered inside the quote; it now renders as plain text |
| [taskEB-review-followup-20261108.md](taskEB-review-followup-20261108.md) | L19 "Not merged, not pushed." by "Merged into devel in wave 64 (Nov 8)" and the Nov 9 status sentence | none | none |
| [writeback-ownership-20261105.md](writeback-ownership-20261105.md) | L11–12 "Not merged, not pushed." by "Merged into devel in wave 60 (Nov 7); its `ready` race … fixed by Task DT and merged in wave 61 (Nov 7)" and the Nov 9 status sentence | none | The quoted "Current status (Nov 8, Task EB)" pointer below the verdict stays as it was (the brief says nothing else changes on task pages); it agrees with the new line |
| [find-res-ownership-20261108.md](find-res-ownership-20261108.md) | L15 "Not merged, not pushed." by "Merged into devel in wave 65 (Nov 9), K101 FIXED" and the Nov 9 status sentence | none | none |
| [navigation-leftovers-20261108.md](navigation-leftovers-20261108.md) | L19 "Not merged, not pushed." by "Merged into devel in wave 65 (Nov 9)", the note that merge-time commit `3edd95ae1` refreshed the `.open` cell the verdict said would fail `make docs-check`, and the Nov 9 status sentence | none | none |
| [readindex-entry-map-20261108.md](readindex-entry-map-20261108.md) | none (the page had no merge sentence); L3 gained "Merged into devel in wave 65 (Nov 9)" and the Nov 9 status sentence at the end of the verdict paragraph | none | none |
| [README.md](../README.md) W171 row | L28 "Task EB (Nov 8, unmerged) … (merged in wave 64)" became "Task EB (merged in wave 64, Nov 8)"; the four "(Nov 9, merged in wave 65)" clauses became one "wave 65 (Nov 9) merged Task EC, …, Task ED, …, Task DO, …, and Task EE, …" clause | none | Dropped two details now on the linked pages: "found by its before-reading" (K101) and "(71 rows gained second tags)" |

`docs/how-it-works/architecture-full.md` and `architecture.html` were
regenerated (`python3 docs/gen-architecture-full.py`) because they embed
the status index; `make docs-check` refuses a stale copy.

Dates: the brief gives wave 60 as Nov 6; the [ledger](ledger.md) records
waves 60 and 61 as merged into devel Nov 7, and the pages use the ledger
date. Wave numbers of the phase 1 and 2 commits were read from
`git log --merges --ancestry-path <commit>..devel --first-parent devel`
(1a `a46a2502e`, 1b `8f0d4ba43`, 2a `589c8219a`: wave 53; 1c `368f5db5f`,
2c `116383888`: wave 54) and match the ledger's wave 53 and 54 entries.

## Checks

```sh
grep -n "Not merged\|unmerged\|PROPOSED" \
  docs/status/2026-11/{structure-plan-20261102,taskEB-review-followup-20261108,writeback-ownership-20261105,find-res-ownership-20261108,readindex-entry-map-20261108,navigation-leftovers-20261108}.md \
  docs/status/README.md
python3 scripts/check_doc_links.py
python3 scripts/status_layout.py --dry-run
make docs-check
sh scripts/rebuild-code-graph.sh && python3 tests/test_code_graph.py
```

- grep: four lines, all in the plan's `## History` section (the old
  opening line, two in the Nov 8/9 quote, the Nov 2 paragraph).
- `check_doc_links.py`: 0 dangling links. `status_layout.py --dry-run`:
  nothing to move. `make docs-check`: rc 0, including
  `check_guide_counts.py` (13 guides match the catalogue) and
  `scripts/evidence/do/check_entry_map.py` (32 functions, 0 stale names);
  it failed once with a stale `architecture-full.md` before the
  regeneration above.

## Not done

- The coordinator's wave merge notes live outside the tree; the rule is
  stated on the plan page only, as the brief asks.
- Other dated pages with "not merged" wording (older W171 evidence pages,
  ledger entries) are history and were not touched.
