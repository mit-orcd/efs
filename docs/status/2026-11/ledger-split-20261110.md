# Ledger split into one file per entry (Task T2)

**Status.** Source commit `06bfdb393` (devel, wave 74). Verdict: PASS, no K row, blocks alpha: no. Remaining failures: none. Active run: none. Next action: coordinator converts open branches with `--split`/`--add-line` and updates the places listed under "Outside my file list". Merge state: branch `claude/focused-nobel-e2ee65`, unmerged.

## Purpose

`docs/status/2026-11/ledger.md` was 172 KB of very long lines under one heading; every branch inserted at the same place and every merge conflicted (brief: `structure-review-tasks-20261110.md`, T2). Now each entry is a file in `docs/status/2026-11/ledger/` and `ledger.md` is an 18 KB generated index.

## What changed

- `scripts/gen_ledger.py`: no argument = check (exit 1 and first differing line when `ledger.md` is stale); `--write`; `--split [--input F] [--output-dir D]`; `--add-line F [--output-dir D]`.
- File name `<yyyymmdd>-<nnn>-<slug>.md`; `nnn` numbers the entries of a day by position in the old file (oldest `001`). Index order: day newest first, then `nnn` highest first, then slug. The rule is in `ledger/README.md`.
- 118 entry files plus `README.md`; `make docs-check` runs the check after `check_doc_links.py`.
- Rule 7 of "Common rules" in `structure-review-tasks-20261110.md` now names the file rule.
- The brief said 101 entries; the file at `06bfdb393` has 118 (the count below).

## Date rule (addition 3)

The brief's rule (first `Nov <d>` in the first 400 characters, else the date of the entry below) gives dates that run backwards in three places, because entries merged late mention an earlier day. I changed the rule instead of hand-editing: a date earlier than the entry below is raised to it. `--split` prints each raised entry. On this ledger:

```text
DATE RAISED: entry 4 20261109 -> 20261110: Stage 1 batch D on fcstor010-014 (Task EY)
DATE RAISED: entry 25 20261109 -> 20261110: Batch F harness fixes
DATE RAISED: entry 29 20261109 -> 20261110: orcd-mini batch O on `9bb50fe79` (Task EW)
split 118 entries ... 3 dates raised
```

With the plain rule, three more entries (5, 26, 30) were flagged only as neighbours of these; they are fine after the raise.

## Commands and results

```bash
awk '/^## Recent evidence/{f=1;next} f&&NF' docs/status/2026-11/ledger.md | wc -l      # before: 118
python3 scripts/gen_ledger.py --split && python3 scripts/gen_ledger.py --write
python3 scripts/gen_ledger.py; echo rc=$?                                              # rc=0
ls docs/status/2026-11/ledger | grep -c '^2'                                           # 118 entry files
wc -c docs/status/2026-11/ledger.md                                                    # 18377 bytes (limit 25 KB)
python3 scripts/check_doc_links.py   # 0 dangling link(s), 0 orphan script(s)
python3 scripts/status_layout.py     # nothing to move
python3 scripts/check_k_register.py  # PASS, 112 rows
make docs-check                      # passes
```

Round-trip check (every original line is contained in the entry files once one `../` is removed from link targets):

```python
import re,glob,sys
old=[l for l in open(sys.argv[1]).read().split("\n")[5:] if l.strip()]
txt="\n".join(open(f).read() for f in glob.glob(sys.argv[2]+"/2*.md"))
txt=re.sub(r"\]\(\.\./", "](", txt)
print(len(old),"entries,",len([l for l in old if l not in txt]),"missing")
```

Run with the old `ledger.md` (`git show 06bfdb393:docs/status/2026-11/ledger.md`) and `docs/status/2026-11/ledger`: `118 entries, 0 missing`. (The script's own entry file for this task was added after that run.)

## Converting an old ledger (addition 4)

```bash
T=$(mktemp -d); git show devel~40:docs/status/2026-11/ledger.md > $T/old.md
python3 scripts/gen_ledger.py --split --input $T/old.md --output-dir $T/out
ls $T/out | wc -l; git status --short   # 96 files; tree unchanged by it
```

Result: `split 96 entries ... 2 dates raised`, 96 files in `$T/out`, and `git status` shows only my own pending changes (nothing from the run). `--add-line` on a scratch copy (not committed):

```bash
echo '[Scratch demo](scratch-demo-20261110.md): **PASS, demo** (Nov 10, branch x). Text.' > $T/line.txt
python3 scripts/gen_ledger.py --add-line $T/line.txt --output-dir $T/out2   # 20261110-001-scratch-demo.md
python3 scripts/gen_ledger.py --add-line $T/line.txt --output-dir $T/out2   # 20261110-002-scratch-demo.md
```

The second call took the next free number of the day; the file body starts `# Scratch demo` and links `../scratch-demo-20261110.md`.

## Outside my file list (for the coordinator)

- `docs/status/README.md:32` says to append entries "directly under its `## Recent evidence and remaining acceptance` heading, newest first"; it should point to `2026-11/ledger/README.md`. Line 17 still correctly links `2026-11/ledger.md`.
- The October ledger and any open-branch ledger edits keep the old layout; convert open branches with `--split --input` / `--add-line`.
- The merge-conflict resolver for the ledger is no longer needed.
- No script or test parses `ledger.md` (`git grep` over `scripts tests Makefile AGENTS.md CLAUDE.md docs/how-it-works` finds none).

## Limitations

- Slugs of the "Wave N merge" entries are all `alpha-1.0-2plus1-checklist` (the page they link); the number keeps names unique, but the slug does not tell them apart.
- `--split` only accepts the old layout (one line per entry under the heading); the date rule needs `Nov <d>` text or falls back to the entry below.
- The index shows the first bold span, not a full summary.
