# K register rows

One file per row, `K<NNN>.md` (three digits, `K001.md`), is the source of the
K register in [the checklist](../alpha-1.0-2plus1-checklist.md). The register
table and counts line there are generated from these files.

```text
# K<N>: <title>

- State: <FIXED|VERIFIED|VERIFY|DECIDED|CLOSED|OPEN>
- Blocks alpha: <yes|no|unstated>
- Kind: <kind>

## Summary
- Cause: <current cause, one sentence>
- Owners: <component tags or source files>
- Reproducer: <test or command>
- Invariant: <rule that must hold>
- Owed: <test, gate or decision still required>

## Item
## Disposition
## Finding on devel
```

- `## Summary` is required for OPEN and VERIFY rows, directly before `## Item`:
  five one-line values, each whole line (label included) at most 220
  characters, no `|`, never wrapped. Rewrite it whenever the State, the cause
  or what is owed changes (a gate that ran, a fix that merged); history stays
  in the three sections below. Write "not determined" for a value the row does
  not give, and say "inferred" for a cause the row only infers. The block may
  stay on a row that becomes FIXED, but it must then say so.
- The title names the defect, not the run that found it; the whole first line
  is at most 80 characters.
- Add a row: copy the layout into the next free number, then run
  `python3 scripts/check_k_register.py --write`.
- Change a state: edit the State line, then run the same command.
- Two branches add the same number: the later merge renames its file (and its
  `# K<N>:` heading) to the next free number and reruns the command.
- `python3 scripts/check_k_register.py` (also in `make docs-check`) fails on a
  gap, a duplicate, a bad State word, a stale generated table, or an OPEN or
  VERIFY row without a well-formed `## Summary`.
- Never put `|` in the title or Kind line (the generator replaces it with `/`).
- `scripts/evidence/t1/split_k_register.py [CHECKLIST] [OUTDIR]` split the old
  two-table layout once; it converts a checklist of that layout.
