#!/usr/bin/env python3
"""check_guide_counts.py [GUIDE...]: the catalogue claims in the component guides
(src/**/README.md) match scripts/tests_for.py on tests/budgets.tsv (Task ED).

Checked, per guide line:
- every `tests_for.py TAG` command names a tag that resolves to rows;
- "`tests_for.py TAG` ... all N" states len(tests_for TAG);
- "<Number> catalogue rows ... the bare `TAG` tag" counts the rows tagged TAG itself;
- in a file table under "tests_for.py BASE.<file>", a Tests cell "`.x` (N" states
  len(tests_for BASE.x), a bare "`.x`" needs at least one row, and "bare tag" or
  "none routed" means BASE.<file stem> has no row of its own.
Not checked: numbers in other wording, counts outside src/**/README.md, and
whether the tags themselves are right (that is the tag audit's job).
Prints file:line stated/actual for each drift; exit 1 on any, 0 otherwise.
"""
import re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tests_for  # noqa: E402

root = tests_for.root
WORDS = {w: i for i, w in enumerate('zero one two three four five six seven eight nine ten eleven twelve'.split())}


def check(guide, rows):
    out, base = [], None
    n = lambda tag: len(tests_for.select(rows, tag))
    for i, line in enumerate(guide.read_text().splitlines(), 1):
        where = '%s:%d' % (guide.relative_to(root) if guide.is_relative_to(root) else guide, i)
        m = re.search(r'tests_for\.py ([\w/]+)\.<file>', line)
        if m:
            base = m.group(1)
        for tag in re.findall(r'tests_for\.py ([\w/]+(?:\.\w+)*)(?![\w./<])', line):
            try:
                tests_for.resolve(rows, tag)
            except LookupError as e:
                out.append('%s: `tests_for.py %s` selects nothing: %s' % (where, tag, e))
        m = re.search(r'tests_for\.py ([\w/]+(?:\.\w+)*)`.*?\ball (\d+)\b', line)
        if m and n(m.group(1)) != int(m.group(2)):
            out.append('%s: %s states all %s, catalogue %d' % (where, m.group(1), m.group(2), n(m.group(1))))
        m = re.search(r'\b(%s) catalogue rows\b.*?bare `([\w/.]+)` tag' % '|'.join(WORDS), line, re.I)
        if m:
            bare = sum(m.group(2) in tags for _, _, tags in rows)
            if bare != WORDS[m.group(1).lower()]:
                out.append('%s: states %s rows with the bare %s tag, catalogue %d' % (where, m.group(1), m.group(2), bare))
        cells = [c.strip() for c in line.split('|')]
        if base and line.startswith('|') and len(cells) > 3 and cells[2].startswith('`'):
            stem, tests = cells[2].strip('`').rsplit('.', 1)[0], cells[-2]
            m = re.match(r'`\.(\w+)`(?: \((\d+))?', tests)
            if m:
                tag, got = base + '.' + m.group(1), n(base + '.' + m.group(1))
                if (m.group(2) and got != int(m.group(2))) or (not m.group(2) and not got):
                    out.append('%s: %s states %s, catalogue %d' % (where, tag, m.group(2) or 'rows', got))
            elif tests.startswith(('bare tag', 'none routed')) and n(base + '.' + stem):
                out.append('%s: %s states no rows of its own, catalogue %d' % (where, base + '.' + stem, n(base + '.' + stem)))
    return out


def main(argv):
    rows = list(tests_for.catalogue())
    guides = [Path(a).resolve() for a in argv] or sorted((root / 'src').glob('**/README.md'))
    drift = [d for g in guides for d in check(g, rows)]
    print('\n'.join(drift) or 'guide counts: %d guides match the catalogue' % len(guides))
    return 1 if drift else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
