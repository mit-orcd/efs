#!/usr/bin/env python3
"""Ledger as one file per entry; ledger.md is a generated index.

  gen_ledger.py                 exit 1 if ledger.md differs from --write output
  gen_ledger.py --write         regenerate ledger.md from ledger/*.md
  gen_ledger.py --split         convert an old-layout ledger (one line per entry)
                                into entry files (--input, --output-dir)
  gen_ledger.py --add-line F    write the one old-style entry line in F as the
                                newest entry of its day (--output-dir)

Entry file: ledger/<yyyymmdd>-<nnn>-<slug>.md = "# title", blank line, one
paragraph. Index order: day newest first, then nnn highest first, then slug.
"""
import argparse, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIR = os.path.join(ROOT, "docs/status/2026-11")
HEADING = "## Recent evidence and remaining acceptance"
NAME = re.compile(r"^(\d{8})-(\d{3})-(.+)\.md$")
LINK = re.compile(r"\[([^\]]*)\]\(([^)]*)\)")


def old_entries(path):
    """Entry lines of an old-layout ledger, newest first."""
    lines = open(path, encoding="utf-8").read().split("\n")
    if HEADING not in lines:
        sys.exit(f"{path}: no '{HEADING}' heading")
    return [l for l in lines[lines.index(HEADING) + 1:] if l.strip()]


def line_date(line):
    m = re.search(r"Nov (\d{1,2})\b", line[:400])
    return f"202611{int(m.group(1)):02d}" if m else None


def link_title(line):
    m = LINK.search(line)
    if not m:
        sys.exit(f"entry without a link: {line[:100]}")
    return m.group(1), m.group(2)


def slug_of(target):
    base = os.path.basename(target.split("#")[0])
    base = re.sub(r"\.md$", "", base)
    base = re.sub(r"-2026\d{4}$", "", base)
    return re.sub(r"[^A-Za-z0-9._-]+", "-", base).strip("-") or "entry"


def rebase(line, prefix):
    def f(m):
        t = m.group(2)
        if t.startswith(("http", "#", "/")) or not t:
            return m.group(0)
        return f"[{m.group(1)}]({prefix}{t})"
    return LINK.sub(f, line)


def unrebase(line):
    def f(m):
        t = m.group(2)
        return f"[{m.group(1)}]({t[3:]})" if t.startswith("../") else m.group(0)
    return LINK.sub(f, line)


def entry_file(outdir, day, n, line):
    title, target = link_title(line)
    name = f"{day}-{n:03d}-{slug_of(target)}.md"
    with open(os.path.join(outdir, name), "w", encoding="utf-8") as fh:
        fh.write(f"# {title}\n\n{rebase(line, '../')}\n")
    return name


def split(inp, outdir):
    entries = old_entries(inp)
    days = []
    for l in reversed(entries):  # oldest first: undated take the date below
        days.append(line_date(l))
    # undated take the date of the entry below (older); the oldest -> 20261101;
    # a date earlier than the entry below is raised to it (entries merged late
    # mention an earlier day), so days never run backwards up the file
    filled, below, raised = [], "20261101", []
    for i, d in zip(range(len(entries) - 1, -1, -1), days):
        if d and d < below:
            raised.append((i + 1, d, below))
        below = max(d or below, below)
        filled.append(below)
    filled.reverse()  # newest first, as entries
    for n, d, to in sorted(raised):
        print(f"DATE RAISED: entry {n} {d} -> {to}: {entries[n - 1][:90]}")
    bad = len(raised)
    os.makedirs(outdir, exist_ok=True)
    count = {}
    for l, d in zip(reversed(entries), reversed(filled)):  # oldest first
        count[d] = count.get(d, 0) + 1
        entry_file(outdir, d, count[d], l)
    print(f"split {len(entries)} entries into {outdir}; {bad} dates raised")


def add_line(path, outdir):
    line = " ".join(open(path, encoding="utf-8").read().split("\n")).strip()
    day = line_date(line)
    if not day:
        sys.exit("no 'Nov <d>' in the first 400 characters of the line")
    os.makedirs(outdir, exist_ok=True)
    nums = [int(m.group(2)) for f in os.listdir(outdir)
            if (m := NAME.match(f)) and m.group(1) == day]
    print(entry_file(outdir, day, max(nums, default=0) + 1, line))


def index(ledger, ldir):
    head = open(ledger, encoding="utf-8").read().split("\n")
    head = head[:head.index(HEADING) + 1]
    files = []
    for f in os.listdir(ldir):
        m = NAME.match(f)
        if m:
            files.append((-int(m.group(1)), -int(m.group(2)), m.group(3), f))
    out = head + [""]
    for _, _, _, f in sorted(files):
        body = open(os.path.join(ldir, f), encoding="utf-8").read().split("\n")
        title = body[0][2:] if body[0].startswith("# ") else sys.exit(f"{f}: no title line")
        text = " ".join(l for l in body[1:] if l.strip())
        v = re.search(r"\*\*(.+?)\*\*", text)
        out.append(f"- [{title}](ledger/{f})" + (f": **{v.group(1)}**" if v else ""))
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--split", action="store_true")
    ap.add_argument("--add-line", metavar="FILE")
    ap.add_argument("--input", default=os.path.join(DIR, "ledger.md"))
    ap.add_argument("--output-dir", default=os.path.join(DIR, "ledger"))
    a = ap.parse_args()
    ledger = os.path.join(DIR, "ledger.md")
    if a.split:
        split(a.input, a.output_dir)
    elif a.add_line:
        add_line(a.add_line, a.output_dir)
    elif a.write:
        text = index(ledger, a.output_dir)  # before open("w") truncates
        open(ledger, "w", encoding="utf-8").write(text)
    else:
        want = index(ledger, a.output_dir).split("\n")
        have = open(ledger, encoding="utf-8").read().split("\n")
        for i in range(max(len(want), len(have))):
            w = want[i] if i < len(want) else None
            h = have[i] if i < len(have) else None
            if w != h:
                print(f"ledger.md line {i + 1} differs; run gen_ledger.py --write\n"
                      f"  have: {(h or '')[:120]}\n  want: {(w or '')[:120]}")
                sys.exit(1)


if __name__ == "__main__":
    main()
