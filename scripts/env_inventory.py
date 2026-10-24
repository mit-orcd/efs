#!/usr/bin/env python3
"""A6: list every environment variable the product reads.

Scans src/ and include/ (efsd, efs-fuse, efs-mgmt, efs-bench and the
libraries). A read is getenv()/secure_getenv() of a string literal or of a
macro naming one, or a call that passes a literal to a name-taking helper:
any function that hands one of its parameters to getenv() or to another such
helper (env_u64("EFS_..."), budget_env(), env_on(), ...). Functions that
walk environ are reported with the prefixes they compare against. Compile-
time modes are the EFS_* names tested by #if/#ifdef/#ifndef (header guards
excepted). The removed settings are the rows of removed_env[] in
src/common/common.c (efs_env_removed_refuse).

  env_inventory.py [--root DIR]               NAME file:line ... per variable
  env_inventory.py [--root DIR] --json        full facts
  env_inventory.py [--root DIR] --uses NAME.. where tests/, scripts/, Makefile use NAME
  env_inventory.py [--root DIR] --check DOC   the make test gate

--check fails when a variable read by the product, an environ scanner, a
compile-time mode or a removed setting has no row in DOC (a table row whose
first cell names it in backquotes); when a removed setting is still read;
when an EFS_FAULT_* variable is read outside #if EFS_FAULTS; or when a read
that exists only under EFS_FAULTS is not named EFS_FAULT_*. A new EFS_*
reader therefore needs a row in docs/status/a6-settings-inventory-20261024.md.
"""
import bisect
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRODUCT_DIRS = ("src", "include")
GETENV = ("getenv", "secure_getenv")
NAME_RE = re.compile(r'"(EFS_[A-Z0-9_]*)"')
FUNC_RE = re.compile(
    r'^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*\{', re.M)
CALL_RE = re.compile(r'\b([A-Za-z_]\w*)\s*\(')


TOKEN_RE = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|/\*.*?\*/|//[^\n]*', re.S)


def strip_comments(text):
    """Blank out comments, keep strings and line numbers."""
    def blank(m):
        t = m.group(0)
        return t if t[0] in "\"'" else re.sub(r'[^\n]', ' ', t)
    return TOKEN_RE.sub(blank, text)


def split_args(text, start):
    """Arguments of the call whose '(' is at text[start]; returns list, end."""
    depth, args, cur, i = 0, [], [], start
    while i < len(text):
        c = text[i]
        if c == '"':
            j = i + 1
            while j < len(text) and text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            cur.append(text[i:j + 1])
            i = j + 1
            continue
        if c in "([{":
            depth += 1
            if depth > 1:
                cur.append(c)
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(''.join(cur).strip())
                return args, i
            cur.append(c)
        elif c == ',' and depth == 1:
            args.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    return args, i


def param_names(params):
    names = []
    for p in params.split(','):
        m = re.search(r'([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*$', p.strip())
        names.append(m.group(1) if m else '')
    return names


class Source:
    def __init__(self, rel):
        self.rel = rel
        with open(os.path.join(ROOT, rel), encoding='utf-8', errors='replace') as f:
            self.text = strip_comments(f.read())
        self.funcs = [(m.start(), m.group(1), param_names(m.group(2)))
                      for m in FUNC_RE.finditer(self.text)
                      if m.group(1) not in ("if", "for", "while", "switch")]
        self.starts = [f[0] for f in self.funcs]
        self.macros = dict(re.findall(
            r'^\s*#\s*define\s+([A-Za-z_]\w*)\s+"(EFS_[A-Z0-9_]*)"', self.text, re.M))

        self.gates = self.preprocessor_gates()

    def preprocessor_gates(self):
        """Per line: the #if conditions in force ('!' marks an #else arm)."""
        gates, stack = [()], []
        for ln in self.text.split('\n'):
            m = re.match(r'\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', ln)
            if m:
                kw, cond = m.group(1), ' '.join(m.group(2).split())
                if kw == 'ifdef':
                    stack.append('defined(%s)' % cond)
                elif kw == 'ifndef':
                    stack.append('!defined(%s)' % cond)
                elif kw == 'if':
                    stack.append(cond)
                elif kw in ('else', 'elif') and stack:
                    stack[-1] = '!(%s)' % stack[-1].lstrip('!') if kw == 'else' else cond
                elif kw == 'endif' and stack:
                    stack.pop()
            gates.append(tuple(stack))
        return gates

    def fault_gated(self, pos):
        return any(re.fullmatch(r'(defined\()?EFS_FAULTS\)?', g) for g in self.gates[self.line(pos)])

    def line(self, pos):
        return self.text.count('\n', 0, pos) + 1

    def enclosing(self, pos):
        i = bisect.bisect_right(self.starts, pos) - 1
        return self.funcs[i][1:] if i >= 0 else None

    def calls(self, wanted):
        for m in CALL_RE.finditer(self.text):
            if m.group(1) not in wanted:
                continue
            args, _ = split_args(self.text, m.end() - 1)
            yield m.group(1), args, m.start()


def set_root(path):
    global ROOT
    ROOT = os.path.abspath(path)


def product_sources():
    for d in PRODUCT_DIRS:
        for base, _, files in os.walk(os.path.join(ROOT, d)):
            for f in sorted(files):
                if f.endswith(('.c', '.h')):
                    yield os.path.relpath(os.path.join(base, f), ROOT)


def scan():
    sources = [Source(rel) for rel in sorted(product_sources())]
    # helper name -> set of parameter indexes that name an environment variable
    helpers = {g: {0} for g in GETENV}
    changed = True
    while changed:
        changed = False
        for s in sources:
            for name, args, pos in s.calls(helpers):
                enc = s.enclosing(pos)
                if not enc:
                    continue
                fn, params = enc
                for idx in helpers[name]:
                    if idx < len(args) and args[idx] in params:
                        pidx = params.index(args[idx])
                        if pidx not in helpers.setdefault(fn, set()):
                            helpers[fn].add(pidx)
                            changed = True
    reads = {}
    for s in sources:
        for name, args, pos in s.calls(helpers):
            for idx in helpers[name]:
                if idx >= len(args):
                    continue
                arg = args[idx]
                m = re.fullmatch(r'"(EFS_[A-Z0-9_]*)"', arg)
                var = m.group(1) if m else s.macros.get(arg)
                if not var:
                    continue
                enc = s.enclosing(pos)
                reads.setdefault(var, []).append({
                    "file": s.rel, "line": s.line(pos), "via": name,
                    "function": enc[0] if enc else None,
                    "faults_only": s.fault_gated(pos)})
    scanners = []
    for s in sources:
        for m in re.finditer(r'\benviron\b', s.text):
            if re.search(r'\bextern\b[^\n]*$', s.text[:m.start()].rsplit('\n', 1)[-1]):
                continue
            enc = s.enclosing(m.start())
            if not enc:
                continue
            key = "%s:%s" % (s.rel, enc[0])
            if any(x["scanner"] == key for x in scanners):
                continue
            body_start = max(st for st, _, _ in s.funcs if st <= m.start())
            _, body_end = split_args(s.text, s.text.index('{', body_start))
            prefixes = sorted(set(re.findall(r'"(EFS_[A-Z0-9_]*)', s.text[body_start:body_end])))
            scanners.append({"scanner": key, "line": s.line(m.start()),
                             "prefixes": prefixes})
    mentions = {}
    for s in sources:
        for m in NAME_RE.finditer(s.text):
            var = m.group(1)
            if var in reads:
                continue
            mentions.setdefault(var, []).append("%s:%d" % (s.rel, s.line(m.start())))
    modes = {}
    for s in sources:
        lines = s.text.split('\n')
        for i, ln in enumerate(lines):
            m = re.match(r'\s*#\s*(if|ifdef|ifndef|elif)\b(.*)', ln)
            if not m:
                continue
            for var in re.findall(r'\b(EFS_[A-Z0-9_]+)\b', m.group(2)):
                nxt = lines[i + 1] if i + 1 < len(lines) else ''
                if m.group(1) == 'ifndef' and re.fullmatch(
                        r'\s*#\s*define\s+%s\s*' % var, nxt):
                    continue  # header guard
                loc = "%s:%d" % (s.rel, i + 1)
                if loc not in modes.setdefault(var, []):
                    modes[var].append(loc)
    removed = []
    common = os.path.join(ROOT, "src/common/common.c")
    if os.path.exists(common):
        with open(common, encoding='utf-8') as f:
            text = f.read()
        m = re.search(r'removed_env\[\] = \{(.*?)\n\};', text, re.S)
        removed = re.findall(r'\{ "(EFS_[A-Z0-9_]+)"', m.group(1)) if m else []
    real_helpers = {k: sorted(v) for k, v in helpers.items() if k not in GETENV}
    return {"reads": reads, "helpers": real_helpers, "scanners": scanners,
            "mentions": mentions, "modes": modes, "removed": removed}


def problems(facts, listed, doc):
    out = []
    for v in sorted(facts["reads"]):
        if v not in listed:
            out.append("%s is read by the product but has no entry in %s" % (v, doc))
    for sc in facts["scanners"]:
        if sc["scanner"] not in listed:
            out.append("environ scanner %s has no entry in %s" % (sc["scanner"], doc))
    for v in sorted(facts["modes"]):
        if v not in listed:
            out.append("compile-time mode %s (%s) has no entry in %s"
                       % (v, facts["modes"][v][0], doc))
    for v in facts["removed"]:
        if v not in listed:
            out.append("removed setting %s has no entry in %s" % (v, doc))
        if v in facts["reads"]:
            r = facts["reads"][v][0]
            out.append("removed setting %s is still read at %s:%d" % (v, r["file"], r["line"]))
    for v, rs in sorted(facts["reads"].items()):
        gated = [r["faults_only"] for r in rs]
        if v.startswith("EFS_FAULT_") and not all(gated):
            r = rs[gated.index(False)]
            out.append("fault hook %s is read outside #if EFS_FAULTS at %s:%d"
                       % (v, r["file"], r["line"]))
        if all(gated) and not v.startswith("EFS_FAULT_"):
            out.append("%s exists only under EFS_FAULTS (%s:%d); name it EFS_FAULT_*"
                       % (v, rs[0]["file"], rs[0]["line"]))
    return out


USE_DIRS = ("tests", "scripts", "tools", "Makefile", "efs-bench.sh")


def uses(names):
    pat = re.compile(r'\b(' + '|'.join(re.escape(n) for n in names) + r')\b')
    found = {n: set() for n in names}
    for d in USE_DIRS:
        p = os.path.join(ROOT, d)
        walk = [(os.path.dirname(p), [], [os.path.basename(p)])] if os.path.isfile(p) else os.walk(p)
        for base, _, files in walk:
            for f in files:
                path = os.path.join(base, f)
                rel = os.path.relpath(path, ROOT)
                if rel == "scripts/env_inventory.py":
                    continue
                try:
                    with open(path, encoding='utf-8', errors='replace') as fh:
                        text = fh.read()
                except OSError:
                    continue
                for m in pat.finditer(text):
                    found[m.group(1)].add(rel)
    return {n: sorted(v) for n, v in found.items()}


def inventory_names(doc):
    names = set()
    with open(doc, encoding='utf-8') as f:
        for line in f:
            m = re.match(r'\|\s*`([^`]+)`', line)
            if m:
                names.add(m.group(1))
    return names


def main(argv):
    args = argv[1:]
    if args[:1] == ["--root"]:
        set_root(args[1])
        args = args[2:]
    facts = scan()
    if args[:1] == ["--json"]:
        json.dump(facts, sys.stdout, indent=1, sort_keys=True)
        print()
        return 0
    if args[:1] == ["--uses"]:
        names = args[1:] or sorted(facts["reads"])
        for n, files in uses(names).items():
            print("%s %s" % (n, " ".join(files) or "-"))
        return 0
    if args[:1] == ["--check"] and len(args) == 2:
        doc = args[1]
        out = problems(facts, inventory_names(doc), os.path.basename(doc))
        for line in out:
            print("env_inventory: " + line)
        print("env_inventory: %d variables read, %d environ scanners, %d compile-time modes, "
              "%d removed; %s" % (len(facts["reads"]), len(facts["scanners"]), len(facts["modes"]),
                                   len(facts["removed"]), "%d problems" % len(out) if out else "PASS"))
        return 1 if out else 0
    if args:
        print(__doc__, file=sys.stderr)
        return 2
    for v in sorted(facts["reads"]):
        locs = " ".join("%s:%d" % (r["file"], r["line"]) for r in facts["reads"][v])
        print("%s %s" % (v, locs))
    for sc in facts["scanners"]:
        print("environ %s:%d prefixes %s" % (sc["scanner"], sc["line"], ",".join(sc["prefixes"])))
    for v in sorted(facts["modes"]):
        print("mode %s %s" % (v, " ".join(facts["modes"][v])))
    for v in facts["removed"]:
        print("removed %s" % v)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
