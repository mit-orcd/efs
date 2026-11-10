# Ledger entries

One file per entry; `../ledger.md` is a generated index. Do not edit it by hand.

To add an entry:

1. Create `<yyyymmdd>-<nnn>-<slug>.md` here. `<yyyymmdd>` is the day of the
   entry; `<nnn>` is the next free three-digit number of that day (the first
   entry of a day is `001`); `<slug>` is the page name without date.
2. The file is `# Title`, a blank line, and one paragraph. Links are relative
   to this directory: a status page is `../` plus its name. Start the
   paragraph with a link to the page, a colon and a bold verdict; the first bold span is the
   verdict shown in the index.
3. Run `python3 scripts/gen_ledger.py --write`. `make docs-check` runs the
   check without arguments and fails when the index is stale.

Order: the index lists the newest day first and, inside a day, the highest
number first. Two branches that take the same number do not conflict because
the slugs differ; the index then orders them by slug.

Converting an old-layout ledger or one added line:
`gen_ledger.py --split --input <old ledger.md> --output-dir <dir>` and
`gen_ledger.py --add-line <file with one entry line>`.
