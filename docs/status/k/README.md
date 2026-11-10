# K register rows

One file per row, `K<NNN>.md` (three digits, `K001.md`), is the source of the
K register in [the checklist](../alpha-1.0-2plus1-checklist.md). The register
table and counts line there are generated from these files.

```text
# K<N>: <title>

- State: <FIXED|VERIFIED|VERIFY|DECIDED|CLOSED|OPEN>
- Blocks alpha: <yes|no|unstated>
- Kind: <kind>

## Item
## Disposition
## Finding on devel
```

- Add a row: copy the layout into the next free number, then run
  `python3 scripts/check_k_register.py --write`.
- Change a state: edit the State line, then run the same command.
- Two branches add the same number: the later merge renames its file (and its
  `# K<N>:` heading) to the next free number and reruns the command.
- `python3 scripts/check_k_register.py` (also in `make docs-check`) fails on a
  gap, a duplicate, a bad State word or a stale generated table.
- Never put `|` in the title or Kind line; the generator replaces it with `/`.
- `scripts/evidence/t1/split_k_register.py [CHECKLIST] [OUTDIR]` split the old
  two-table layout once; it converts a checklist of that layout.
