# K row summaries (Nov 10, 2026)

**Merge state (Nov 10):** merged into devel in wave 78 after an independent review of all 42 summaries against the newest dated statements of their rows. The merge corrected 39 lines in 24 rows. Five of them stated something the row contradicts: the titles of K17 and K18, the Owed lines of K2 and K15, and the Cause of K33. It gave K106, K109, K112 and K113 defect titles, added a dated line on the held fix to row K109, and extended `docs/status/k/README.md` (when to rewrite the block, "not determined" and "inferred", the title rule). The summaries of K106, K109, K112 and K113 describe devel before wave 77 and are rewritten when that wave merges.

- Source commit: devel `67fc3b4a1` plus the commits of branch `claude/task-u3-k-summaries-54f183` (Task U3).
- Verdict: PASS (documentation and one checker; no source code read, no host used).
- Remaining failures: none.
- Active run: none.
- Next action: the coordinator reviews and merges; at the wave 77 merge the summaries of K106, K109, K112 and K113 are updated to the new State lines.
- Merge state: branch `claude/task-u3-k-summaries-54f183`, unmerged.

## Purpose

An external structure review of devel `67fc3b4a1` found that the K row files open with long repeated incident history and carry titles cut from the incident text. Each OPEN or VERIFY row now starts with a five-line `## Summary` (Cause, Owners, Reproducer, Invariant, Owed) directly before `## Item`, and the title names the defect. History stays in the three sections below.

## What changed

- `scripts/check_k_register.py` fails an OPEN or VERIFY row whose Summary is missing, not directly before `## Item`, lacks a label, has them out of order, has an empty value, a line over 220 characters or a `|`. Other states may have the section. `--write`, `--json` and the `PASS K register: ...` line are unchanged.
- `tests/test_k_register.py` has three new tests with four cases: an OPEN row without the section fails; a VERIFY row missing a label fails; a 225-character line fails; a FIXED row without the section passes.
- 42 rows got a Summary; 38 got a new title (K106, K109, K112 and K113 kept their title lines by instruction). K106, K109, K112 and K113 were written from devel as it is, to be updated at the wave 77 merge. `docs/status/k/README.md` documents the section (38 lines).
- The summaries were written only from each row's three sections. Every name in backticks occurs in the row (checked by script). Where the row states a part, the value says so with the fixed phrase.

## Commands and results

| Command | Result |
| --- | --- |
| Baseline `python3 scripts/check_k_register.py` (before) | `PASS K register: 113 rows; FIXED=43, VERIFIED=21, VERIFY=37, DECIDED=4, CLOSED=3, OPEN=5` |
| Same command after the change | `PASS K register: 113 rows; FIXED=43, VERIFIED=21, VERIFY=37, DECIDED=4, CLOSED=3, OPEN=5` |
| `grep -lE '^- State: (OPEN\|VERIFY)$' docs/status/k/*.md` | 42 files (5 OPEN, 37 VERIFY), as expected |
| `python3 tests/test_k_register.py` | 10 tests OK (7 old, 3 new) |
| `git diff devel --stat -- docs/status/k` | the 42 row files and the README only |
| Per-row diff line counts (added/removed) | K11: 9/1, K44: 9/1, K106: 8/0 (title kept); each shows only the title line and the added section |

The other checks of the task (`check_doc_links`, `status_layout`, `make docs-check`, code graph) are listed in the report to the coordinator.

Reproduce: check out the branch and run `python3 scripts/check_k_register.py` and `python3 tests/test_k_register.py`. Nothing to clean up.

## Rows with parts left undetermined

24 of 42 rows have at least one part written as a "not ..." value:

| Row | Parts left undetermined |
| --- | --- |
| K4 | Owners |
| K6 | Owners |
| K11 | Cause, Owners |
| K12 | Cause, Owners, Reproducer, Invariant |
| K13 | Owners |
| K14 | Owners |
| K15 | Owners |
| K16 | Owners |
| K17 | Owners |
| K18 | Cause, Owners, Reproducer, Invariant |
| K19 | Cause, Owners |
| K20 | Owners, Reproducer |
| K21 | Cause, Owners, Reproducer, Invariant |
| K29 | Owners |
| K40 | Owners |
| K44 | Owners, Invariant |
| K48 | Owners |
| K49 | Owners |
| K50 | Owners |
| K105 | Owners |
| K110 | Owners |
| K111 | Owners, Reproducer |
| K112 | Owners |
| K113 | Owners |

## Limitations

- Summaries are condensed by hand from long rows; the row sections remain the evidence. A reader should check an Owed line against the newest dated statement in the row.
- Owners are mostly "not named in the row" because the rows name tests and gates more often than components; no source was read to fill them.
- The register table in the checklist could not be regenerated between the first commits (the checker refuses `--write` while a row is incomplete); it is current in the last row commit.
