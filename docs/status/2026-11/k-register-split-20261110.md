# K register split into one file per row (Task T1)

**Merge state (Nov 10):** merged into devel in wave 76. The merge regenerated the 112 row files from the register as it stood then (after the K109 and K112 edits) with this task's splitter; the 170 KB size target was missed by 6 KB and is accepted (the checklist was 428 KB before).

- Source commit: `06bfdb393` (devel, wave 74 ledger entry); branch `claude/charming-williamson-a34687`, the commit that adds this page.
- Verdict: **PASS for the split and the generated table; one done check FAILS: the checklist is 176425 bytes, not under 170 KB** (see Limitations).
- Remaining failures: the 170 KB target (not reachable inside the allowed edits).
- Active run: none. Next action: coordinator decides on the 170 KB target.
- Merge state: `claude/charming-williamson-a34687`, unmerged.

## Purpose

Structure review task T1 (`structure-review-tasks-20261110.md`): the 112-row K register lived in two tables with one very long line per row in `docs/status/alpha-1.0-2plus1-checklist.md`. Now `docs/status/k/K<NNN>.md` is the source and the checklist holds a generated table.

## What changed

- `scripts/evidence/t1/split_k_register.py [CHECKLIST] [OUTDIR]`: mechanical, rerunnable split of an old-layout checklist. It refuses a row whose cell count is not 4 (register) or 2 (recheck), which is what a stray `|` causes. Title rule as briefed, plus one addition: Markdown links are reduced to their text before the cut (otherwise K110's title ended in half a link and the link check failed). `|` in a title is replaced by `/`.
- `scripts/check_k_register.py`: row files are the source; checks no gap, no duplicate, canonical State, Blocks alpha and Kind lines; generates the table and counts line between the two marker lines (`--write`); without `--write` a stale block fails. `--json` and the last line `PASS K register: ...` kept (the PASS line now says `N rows;` instead of `N rows in each table;`).
- `tests/test_k_register.py`: missing number, duplicate number, bad State, stale table, pipe in title, JSON, on temporary copies.
- `docs/status/k/README.md`, 112 row files, the checklist (first table replaced by the markers and generated block, second table replaced by one sentence, "Wave notes (historical; the counts above are generated): " put before the long "Current counts (" sentence). The "Recheck" heading and its introduction above the removed second table were left as they are.

## Commands and results

| Check | Command | Result |
| --- | --- | --- |
| Before | `python3 scripts/check_k_register.py` | `PASS K register: 112 rows in each table; FIXED=43, VERIFIED=21, VERIFY=37, DECIDED=4, CLOSED=3, OPEN=4` |
| After | same | `PASS K register: 112 rows; FIXED=43, VERIFIED=21, VERIFY=37, DECIDED=4, CLOSED=3, OPEN=4` |
| Tests | `python3 tests/test_k_register.py` | 7 tests OK |
| Links | `python3 scripts/check_doc_links.py` | 0 dangling link(s) |
| Docs | `make docs-check`; `python3 scripts/status_layout.py` | rc=0; nothing to move |
| Idempotent | run the splitter twice on the old checklist (`git show 06bfdb393:docs/status/alpha-1.0-2plus1-checklist.md`), compare `git status` and a hash of all row files | identical |
| Old layout | `git show devel~40:docs/status/alpha-1.0-2plus1-checklist.md > $TMP/c.md; python3 scripts/evidence/t1/split_k_register.py $TMP/c.md $TMP/out` | 105 row files in `$TMP/out`, tree untouched, second run identical |
| Size | `wc -c` on the checklist | 424535 bytes before, 176425 after; K rows: 112 |

Callers of `check_k_register` (`git grep -n "check_k_register" -- scripts tests Makefile docs/how-it-works`): `Makefile:507` (`docs-check`, passes) and `tests/budgets.tsv:164` (budget row, command unchanged, runs in well under 30 s; its description still says "two tables", not changed as the file is outside this task); `tests/test_k_register.py` loads the script and runs it as a CLI (passes). Nothing under `docs/how-it-works` calls it.

## Limitations

- The 170 KB target cannot be met inside the allowed edits: the checklist minus the generated block is 161783 bytes (the long "Current counts (" wave-notes line alone is 9139 bytes and must stay), and the generated table is    14642 bytes, so 170 KB would need a table of about 8 KB. Shortening titles would not reach it.
- Blocks alpha is taken from the first `blocks alpha: yes|no` in each cell, the second table's cell winning; 29 yes, 45 no, 38 unstated.
- Not run: the graph rebuild result is in the last commit.
