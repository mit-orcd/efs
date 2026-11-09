#!/usr/bin/env python3
"""tag_audit.py ROOT (Task ED): the catalogue tag audit. Per tests/budgets.tsv row, the src/ implementation files the
test reads (literal src/ paths, csrc.module()/MODULES splices, path joins),
#includes (C, also inside embedded C strings) or links (explicit src/*.c deps
of its Makefile rule; $(LIB) is the whole library and is not counted), plus the
daemons/mounts it drives (recorded, not tagged). Prints TSV: line, command,
tags, components read, uncovered components, drives."""
import re, subprocess, sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(root / 'tests'))
sys.path.insert(0, str(root / 'scripts'))
import csrc  # noqa
import tests_for  # noqa

mk = (root / 'Makefile').read_text()
rules = {}
for m in re.finditer(r'^(tests/test_\w+):([^\n]*)\n((?:\t[^\n]*\n)*)', mk, re.M):
    rules.setdefault(m.group(1), '')
    rules[m.group(1)] += m.group(2) + '\n' + m.group(3)


def comp(path):
    p = Path(path).with_suffix('').parts[1:]  # drop src
    if len(p) == 1:
        return p[0]
    if len(p) == 2:
        return p[0] + '/' + p[1]
    return p[0] + '/' + p[1] + '.' + p[2]


def top(c):
    return c.split('.')[0]


def covered(c, tags):
    for t in tags:
        if '/' not in t:  # a directory tag: kv, raft, common, data, ...
            if c.split('/')[0] == t:
                return True
        elif top(c) == top(t):
            return True
    return False


SRC = re.compile(r'src/[\w/]+\.c\b')
JOIN = re.compile(r"""['"]src['"]\s*/\s*['"](\w+)['"]\s*/\s*['"]([\w.]+)['"](?:\s*/\s*['"]([\w.]+)['"])?""")
JOIN2 = re.compile(r"""['"]src/(\w+)['"]\s*/\s*['"]([\w.]+)['"]""")
MOD = re.compile(r"""(?:module|source|read_module|MODULES\[)\(?\s*['"](\w+)['"]""")


def code_only(text):
    """Drop the module docstring and comment lines: a file named only in prose is not read."""
    import ast
    try:
        doc = ast.get_docstring(ast.parse(text), clean=False)
        if doc:
            text = text.replace(doc, '', 1)
    except SyntaxError:
        pass
    keep = []
    for ln in text.splitlines():
        t = ln.strip()
        if t.startswith(('#', '//', '/*', '* ', '*/')) and not t.startswith('#include'):
            continue
        keep.append(ln)
    return '\n'.join(keep)


def files_read(text):
    text = code_only(text)
    out = set(SRC.findall(text))
    for m in JOIN.finditer(text):
        parts = [g for g in m.groups() if g]
        out.add('src/' + '/'.join(parts))
    for m in JOIN2.finditer(text):
        out.add('src/%s/%s' % m.groups())
    for m in MOD.finditer(text):
        if m.group(1) in csrc.MODULES:
            out.update(f for f in csrc.MODULES[m.group(1)][1] if f.endswith('.c'))
    return {f for f in out if f.endswith('.c') and (root / f).exists()}


DRIVE = {'efsd': re.compile(r'\befsd\b'), 'fuse-mount': re.compile(r'efs_fuse\b|\bmount\b'),
         'efs_mgmt': re.compile(r'\befs_mgmt\b')}

for line, text in enumerate((root / 'tests/budgets.tsv').read_text().splitlines(), 1):
    f = text.split('\t')
    if len(f) < 7 or text.startswith('#') or f[0] == 'command':
        continue
    cmd, target, tags = f[0], f[4], [t for t in f[5].split(',') if t]
    full = cmd
    if cmd.endswith('\\'):
        try:
            full = tests_for.full_command(cmd, target)
        except Exception:
            pass
    read, drives = set(files_read(full)), set()
    for name in set(re.findall(r'(?:tests|scripts)/[\w./-]+', full)):
        cands = [name] if (root / name).is_file() else [name + e for e in ('.c', '.py', '.sh') if (root / (name + e)).is_file()]
        for c in cands:
            t = (root / c).read_text(errors='replace')
            read |= files_read(t)
            for k, rx in DRIVE.items():
                if rx.search(t):
                    drives.add(k)
        if name in rules or name.rstrip('./') in rules:
            read |= {x for x in SRC.findall(rules[name]) if (root / x).exists()}
    comps = sorted({comp(x) for x in read})
    unc = sorted({c for c in comps if not covered(c, tags)})
    print('\t'.join([str(line), cmd[:80], ','.join(tags), ' '.join(comps), ' '.join(unc), ' '.join(sorted(drives))]))
