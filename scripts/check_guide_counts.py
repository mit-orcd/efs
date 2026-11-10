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
- in a contract → case map (a table headed "| Operation or invariant | Cases |
  Command |", Task EU), the Command cell names a tracked `./tests/<bin>` (its
  `<bin>.c`) or a `tests_for.py` fallback; every backticked name in the Cases
  cell is a `RUN(name)` case of `<bin>.c` when the file has RUN lines (and the
  command's arguments select it), else a function defined in `<bin>.c`;
  a "none" row names no case and runs the fallback.
Not checked: whether a named case proves its row (a reader's job), numbers in other wording, counts outside src/**/README.md, and
whether the tags themselves are right (that is the tag audit's job).
Prints file:line stated/actual for each drift; exit 1 on any, 0 otherwise.
"""
import re, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tests_for  # noqa: E402

root = tests_for.root
WORDS = {w: i for i, w in enumerate('zero one two three four five six seven eight nine ten eleven twelve'.split())}


MAP_HEAD = '| Operation or invariant | Cases | Command |'


def tracked():
    out = subprocess.run(['git', 'ls-files', '-z', '--', 'tests'], cwd=root,
                         capture_output=True, check=True).stdout
    return set(out.decode().split('\0'))


def check_map_row(where, cells, files):
    """cells: one map row; files: tracked paths. Returns drift lines."""
    names, cmd = re.findall(r'`(\w+)`', cells[2]), cells[3]
    m = re.search(r'\./tests/(\w+)((?: [\w-]+)*)', cmd)
    if cells[2].startswith('none'):
        if names or 'tests_for.py' not in cmd:
            return ['%s: a "none" row names a case or runs no tests_for.py fallback' % where]
        return []
    if not m:
        return ['%s: %s: command names no ./tests/ binary' % (where, ', '.join(names) or 'no case')]
    src = 'tests/%s.c' % m.group(1)
    if src not in files:
        return ['%s: %s: %s is not a tracked file' % (where, ', '.join(names) or 'no case', src)]
    if not names:
        return ['%s: no case named for %s' % (where, src)]
    text, args, out = (root / src).read_text(), m.group(2).split(), []
    runs = re.findall(r'^\s*RUN\((\w+)\);', text, re.M)
    out += ['%s: argument %s matches no RUN case of %s' % (where, a, src)
            for a in args if runs and not any(a in r for r in runs)]
    for name in names:
        if runs and name not in runs:
            out.append('%s: %s is not a RUN case of %s' % (where, name, src))
        elif runs and args and not any(a in name for a in args):
            out.append('%s: %s is not selected by the command in %s' % (where, name, src))
        elif not runs and not re.search(r'^[A-Za-z][\w \*]*\b%s\([^;]*$' % name, text, re.M):
            out.append('%s: %s is not a function in %s' % (where, name, src))
    return out


def check(guide, rows, files=None):
    out, base, in_map, files = [], None, False, tracked() if files is None else files
    n = lambda tag: len(tests_for.select(rows, tag))
    for i, line in enumerate(guide.read_text().splitlines(), 1):
        where = '%s:%d' % (guide.relative_to(root) if guide.is_relative_to(root) else guide, i)
        if line.strip().startswith(MAP_HEAD):
            in_map, base = True, None
            continue
        in_map = in_map and line.strip().startswith('|')
        if in_map:
            if not set(line.strip()) <= set('|-'):
                out += check_map_row(where, [c.strip() for c in line.strip().split('|')], files)
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
    files = tracked()
    drift = [d for g in guides for d in check(g, rows, files)]
    print('\n'.join(drift) or 'guide counts: %d guides match the catalogue' % len(guides))
    return 1 if drift else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
