#!/usr/bin/env python3
"""Split the old two-table K register of the alpha checklist into docs/status/k/K<NNN>.md.

Usage: python3 scripts/evidence/t1/split_k_register.py [CHECKLIST] [OUTDIR]
Mechanical and rerunnable: the same input gives identical files. Works on any
checklist of the old layout (tables headed `| ID | Item | Kind | Disposition |`
and `| Row | Finding on devel |`); OUTDIR defaults to docs/status/k.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
STATES = ('FIXED', 'VERIFIED', 'VERIFY', 'DECIDED', 'CLOSED', 'OPEN')
HEADERS = {('ID', 'Item', 'Kind', 'Disposition'): 4, ('Row', 'Finding on devel'): 2}


def tables(text):
    """Return ({n: (item, kind, disposition)}, {n: finding}); die on an unparsable row."""
    reg, rec, cols = {}, {}, None
    for no, line in enumerate(text.splitlines(), 1):
        if not line.startswith('|'):
            cols = None
            continue
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        if tuple(cells) in HEADERS:
            cols = HEADERS[tuple(cells)]
            continue
        if cols is None or all(re.fullmatch(r':?-+:?', c) for c in cells):
            continue
        m = re.fullmatch(r'K([1-9][0-9]*)', cells[0])
        # a '|' inside a cell changes the cell count: refuse, never guess
        if not m or len(cells) != cols:
            sys.exit(f'line {no}: row {cells[0]!r} has {len(cells)} cells, expected {cols} (stray "|"?)')
        dest = reg if cols == 4 else rec
        if int(m[1]) in dest:
            sys.exit(f'line {no}: duplicate {cells[0]}')
        dest[int(m[1])] = tuple(cells[1:]) if cols == 4 else cells[1]
    if not reg:
        sys.exit('no K rows found (already split?)')
    if set(reg) != set(rec):
        sys.exit('tables differ in rows: ' + ', '.join(f'K{k}' for k in sorted(set(reg) ^ set(rec))))
    return reg, rec


def relink(s):
    return re.sub(r'\]\((?!http|#|/)([^)\s][^)]*)\)', r'](../\1)', s)


def blocks(*cells):
    found = 'unstated'
    for c in cells:  # the second table's cell comes last and wins
        m = re.search(r'blocks alpha: (yes|no)', c, re.I)
        if m:
            found = m[1].lower()
    return found


def title(item):
    t = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', item).replace('**', '')  # a cut must not leave half a link
    t = t[:min([i for i in (t.find('. '), t.find(': ')) if i >= 0] + [100])]
    return t.replace('|', '/').strip()


def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'docs/status/alpha-1.0-2plus1-checklist.md'
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / 'docs/status/k'
    reg, rec = tables(src.read_text())
    out.mkdir(parents=True, exist_ok=True)
    for n in sorted(reg):
        item, kind, disp = reg[n]
        m = re.match('(' + '|'.join(STATES) + r')(?=$|[\s:.,;(])', disp)
        if not m:
            sys.exit(f'K{n}: Disposition must start with a state word')
        (out / f'K{n:03d}.md').write_text(
            f'# K{n}: {title(item)}\n\n- State: {m[1]}\n- Blocks alpha: {blocks(disp, rec[n])}\n- Kind: {kind}\n\n'
            f'## Item\n\n{relink(item)}\n\n## Disposition\n\n{relink(disp)}\n\n'
            f'## Finding on devel\n\n{relink(rec[n])}\n')
    print(f'wrote {len(reg)} row files to {out}')


if __name__ == '__main__':
    main()
