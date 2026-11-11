#!/usr/bin/env python3
"""lifecycle_inventory.py SOURCE.c

Read-only inventory of four lifecycle candidates in src/server/raft_host.c
(task V2). The function list of each candidate is in candidate_*.txt next to
this script (one name per line). Prints, per candidate: functions and lines,
struct fields used (inside / outside references), locks and atomics, outside
callers, outside static callees, and tests that name each function. Then one
summary table. Option --sites also prints every outside use of a candidate
function (caller:line); --field-sites prints every outside reference to a
field that is used inside (owner:line), for fields with 12 or fewer outside
references; --functions prints name, first line, last line and length of every
candidate function. Exit 1 when a listed name is not defined exactly once with a
body, or a list has a duplicate. Standard library only; writes nothing.
"""
import glob
import os
import re
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
DEF = re.compile(r'^[A-Za-z_][A-Za-z0-9_ \*]*[ \*]([a-z0-9_]+)\([^;]*$')
SKIP = re.compile(r'(if|for|while|switch|return|else)\b')
STRUCTS = ("efs_raft_host", "host_group")
HUB = 100  # a field with this many references in the file is shared infrastructure


def strip(text):
    """Blank comments, string and char literals; keep every newline."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def find_functions(lines):
    """Return ({name: (start, end, static)}, decl_lines, duplicates).
    start/end are 1-based; a definition has '{' before ';' after its first
    line. A multi-line forward declaration is recorded in decl_lines."""
    defs, decl, dups = {}, set(), []
    i, n = 0, len(lines)
    while i < n:
        m = DEF.match(lines[i])
        if not m or SKIP.match(lines[i]):
            i += 1
            continue
        j, kind = i, None
        while j < n:
            ln = lines[j]
            k1, k2 = ln.find("{"), ln.find(";")
            if j == i:
                k2 = ln.find(";") if ";" in ln else -1
            if k1 >= 0 and (k2 < 0 or k1 < k2):
                kind = "def"
                break
            if k2 >= 0:
                kind = "decl"
                break
            j += 1
        if kind == "decl":
            decl.update(range(i + 1, j + 2))
            i = j + 1
            continue
        if kind is None:
            i += 1
            continue
        e = j
        while e < n and not lines[e].startswith("}"):
            e += 1
        name = m.group(1)
        if name in defs:
            dups.append(name)
        defs[name] = (i + 1, e + 1, lines[i].lstrip().startswith("static"))
        i = e + 1
    return defs, decl, dups


def struct_fields(stripped, name):
    m = re.search(r'^struct %s \{' % name, stripped, re.M)
    if not m:
        return []
    e = stripped.index("\n};", m.end())
    fields = []
    for stmt in stripped[m.end():e].split(";"):
        stmt = " ".join(stmt.split())
        if not stmt:
            continue
        for part in stmt.split(","):
            part = re.sub(r'\[[^\]]*\]', '', part)
            part = re.sub(r':\s*\d+\s*$', '', part).strip()
            ids = re.findall(r'[A-Za-z_][A-Za-z0-9_]*', part)
            if ids:
                fields.append(ids[-1])
    return fields


def all_struct_field_names(paths):
    """field name -> set of struct names, for every struct body in paths."""
    seen = defaultdict(set)
    for p in paths:
        try:
            s = strip(open(p, errors="replace").read())
        except OSError:
            continue
        for m in re.finditer(r'\bstruct\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{', s):
            depth, k = 1, m.end()
            while k < len(s) and depth:
                depth += (s[k] == "{") - (s[k] == "}")
                k += 1
            for stmt in s[m.end():k - 1].split(";"):
                stmt = " ".join(re.sub(r'\{[^{}]*\}', '', stmt).split())
                for part in stmt.split(","):
                    part = re.sub(r'\[[^\]]*\]', '', part)
                    part = re.sub(r':\s*\d+\s*$', '', part).strip()
                    ids = re.findall(r'[A-Za-z_][A-Za-z0-9_]*', part)
                    if ids and "(" not in part:
                        seen[ids[-1]].add(m.group(1))
    return seen


def main(argv):
    flags = {a for a in argv[2:] if a.startswith("--")}
    if len(argv) < 2 or len(argv) - len(flags) != 2 or flags - {"--sites", "--field-sites", "--functions"}:
        print(__doc__)
        return 2
    src = argv[1]
    root = os.path.dirname(os.path.dirname(os.path.abspath(src)))
    root = os.path.dirname(root)  # src/server/x.c -> repo root
    raw = open(src, errors="replace").read()
    stripped = strip(raw)
    slines = stripped.split("\n")
    defs, decl, dups = find_functions(slines)

    cands, bad = {}, 0
    for path in sorted(glob.glob(os.path.join(HERE, "candidate_*.txt"))):
        key = os.path.basename(path)[len("candidate_"):-4]
        names = [x.strip() for x in open(path) if x.strip()]
        for d in sorted({x for x in names if names.count(x) > 1}):
            print("FAIL %s: duplicate name %s" % (key, d))
            bad = 1
        for x in names:
            if x not in defs:
                print("FAIL %s: %s is not defined in %s" % (key, x, src))
                bad = 1
            elif x in dups:
                print("FAIL %s: %s is defined more than once" % (key, x))
                bad = 1
        cands[key] = names
    if bad:
        return 1

    # which function does each source line belong to
    owner = {}
    for nm, (s, e, _) in defs.items():
        for ln in range(s, e + 1):
            owner[ln] = nm

    fields = {st: struct_fields(stripped, st) for st in STRUCTS}
    fieldset = {f: [st for st in STRUCTS if f in fields[st]]
                for st in STRUCTS for f in fields[st]}
    headers = glob.glob(os.path.join(root, "include", "**", "*.h"), recursive=True)
    headers += glob.glob(os.path.join(root, "src", "**", "*.[ch]"), recursive=True)
    others = all_struct_field_names(headers)
    # a name is ambiguous when some struct other than the two also has it
    ambiguous = {f for f in fieldset
                 if any(s not in STRUCTS for s in others.get(f, ()))}
    print("source: %s (%d lines); %d functions with a body"
          % (src, len(slines) - 1 if slines[-1] == "" else len(slines), len(defs)))
    print("struct efs_raft_host: %d fields; struct host_group: %d fields"
          % (len(fields["efs_raft_host"]), len(fields["host_group"])))
    print("fields whose name also occurs in another struct (ambiguous): %d of %d"
          % (len(ambiguous), len(fieldset)))
    print("duplicate field names across the two structs: %s"
          % (sorted(f for f in fieldset if len(fieldset[f]) > 1) or "none"))

    # reference positions of every field name, per line (code only).
    # raw: every ->name / .name. refined: for an ambiguous name only a
    # reference whose base expression is known to be one of the two structs.
    hostvars = set(re.findall(r'struct efs_raft_host\s*\*+\s*([a-z_0-9]+)', stripped))
    groupvars = set(re.findall(r'struct host_group\s*\*+\s*([a-z_0-9]+)', stripped))
    groupfuncs = set(re.findall(r'struct host_group\s*\*\s*([a-z_0-9]+)\s*\(', stripped))
    groupvars -= groupfuncs
    groupfuncs.add("group_slot")
    fre = re.compile(r'(->|\.)\s*([A-Za-z_][A-Za-z0-9_]*)')

    def base_type(text, pos):
        """struct name the expression ending before text[pos] has, or None."""
        k = pos
        while k > 0 and text[k - 1] == " ":
            k -= 1
        if k == 0:
            return None
        c = text[k - 1]
        if c in "])":
            op, cl = ("[", "]") if c == "]" else ("(", ")")
            depth, j = 0, k - 1
            while j >= 0:
                depth += (text[j] == cl) - (text[j] == op)
                if depth == 0:
                    break
                j -= 1
            m = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*$', text[:j])
            if not m:
                return None
            if c == "]":
                return "host_group" if m.group(1) == "g" else None
            return "host_group" if m.group(1) in groupfuncs else None
        m = re.search(r'([A-Za-z_][A-Za-z0-9_]*)$', text[:k])
        if not m:
            return None
        v = m.group(1)
        if v in hostvars:
            return "efs_raft_host"
        if v in groupvars:
            return "host_group"
        return None

    refs, raw_refs, unresolved = defaultdict(list), defaultdict(list), Counter()
    for idx, ln in enumerate(slines, 1):
        for m in fre.finditer(ln):
            f = m.group(2)
            if f not in fieldset:
                continue
            raw_refs[f].append(idx)
            if f not in ambiguous:
                refs[f].append(idx)
                continue
            t = base_type(ln, m.start())
            if t is None:
                unresolved[f] += 1
            elif t in fieldset[f]:
                refs[f].append(idx)

    amb_raw = sum(len(raw_refs[f]) for f in ambiguous)
    amb_kept = sum(1 for f in ambiguous for _ in refs[f])
    print("references by name: %d; of them on ambiguous names: %d, kept after the base-type "
          "check: %d, dropped (wrong or unknown base): %d, of which base unknown: %d"
          % (sum(len(v) for v in raw_refs.values()), amb_raw, amb_kept,
             amb_raw - amb_kept, sum(unresolved.values())))

    # file-scope variables (also '} name;' closing a static struct body): a col-0 'static'/'__thread' line with no '(' and a name
    # before '=', ';' or '['
    gvre = re.compile(r'^(?:static|__thread|_Thread_local)\b[^(){}]*?\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*(?:=|;)')
    gvars, opened = {}, False
    for idx, ln in enumerate(slines, 1):
        if idx in owner:
            continue
        m = gvre.match(ln)
        if m:
            gvars.setdefault(m.group(1), idx)
        elif re.match(r'^(?:static|__thread)\b[^()]*\{\s*$', ln):
            opened = True  # a static struct/union body: the variable follows the brace
        elif opened and ln.startswith("}"):
            m = re.match(r'^\}\s*([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*[=;]', ln)
            if m:
                gvars.setdefault(m.group(1), idx)
            opened = False
    gv_refs = defaultdict(list)
    if gvars:
        gre = re.compile(r'\b(%s)\b' % "|".join(sorted(gvars, key=len, reverse=True)))
        for idx, ln in enumerate(slines, 1):
            for m in gre.finditer(ln):
                if gvars[m.group(1)] != idx:
                    gv_refs[m.group(1)].append(idx)
    print("file-scope variables found: %d" % len(gvars))

    # call graph over bodies
    statics = {n for n, v in defs.items() if v[2]}
    name_re = re.compile(r'\b(%s)\b' % "|".join(sorted(defs, key=len, reverse=True)))
    uses = defaultdict(list)  # callee -> [(line, is_call)]
    for idx, ln in enumerate(slines, 1):
        if idx in decl:
            continue
        if idx not in owner and ln[:1].strip() and ln.rstrip().endswith(";") \
                and "(" in ln and not ln.startswith("#"):
            continue  # file-scope declaration, not a use
        for m in name_re.finditer(ln):
            nm = m.group(1)
            if defs[nm][0] == idx:
                continue
            after = ln[m.end():].lstrip()
            uses[nm].append((idx, after.startswith("(")))

    # other source files that name the function (cross-file interface)
    other_src = [p for p in glob.glob(os.path.join(root, "src", "**", "*.[ch]"), recursive=True)
                 if os.path.abspath(p) != os.path.abspath(src)]
    other_txt = {p: strip(open(p, errors="replace").read()) for p in other_src}
    test_txt = {}
    for p in glob.glob(os.path.join(root, "tests", "**", "*"), recursive=True):
        if os.path.isfile(p):
            try:
                test_txt[p] = open(p, errors="replace").read()
            except OSError:
                pass

    rows = {}
    for key, names in cands.items():
        cset = set(names)
        lines_tot = sum(defs[n][1] - defs[n][0] + 1 for n in names)
        print("\n=== Candidate %s: %d functions, %d lines ===" % (key, len(names), lines_tot))
        inside_lines = {ln for n in names for ln in range(defs[n][0], defs[n][1] + 1)}

        if "--functions" in flags:
            for n in names:
                print("     fn %s L%d-L%d %d" % (n, defs[n][0], defs[n][1],
                                                 defs[n][1] - defs[n][0] + 1))
        # b. fields
        used = {}
        for f, lns in refs.items():
            ins = sum(1 for ln in lns if ln in inside_lines)
            if ins:
                used[f] = (ins, len(lns) - ins)
        only_in = sorted(f for f, (i, o) in used.items() if o == 0)
        tin = sum(i for i, o in used.values())
        tall = sum(i + o for i, o in used.values())
        hub = {f for f in fieldset if len(raw_refs.get(f, ())) >= HUB}
        nh = [f for f in used if f not in hub]
        hin = sum(used[f][0] for f in nh)
        hall = sum(sum(used[f]) for f in nh)
        rin = rall = 0
        for f, lns in raw_refs.items():
            ins = sum(1 for ln in lns if ln in inside_lines)
            if ins:
                rin += ins
                rall += len(lns)
        amb_used = [f for f in used if f in ambiguous]
        print("b. fields used: %d (host: %d, group: %d; %d have an ambiguous name); "
              "used only inside: %d"
              % (len(used), sum(1 for f in used if "efs_raft_host" in fieldset[f]),
                 sum(1 for f in used if "host_group" in fieldset[f]),
                 len(amb_used), len(only_in)))
        print("   refs inside %d of %d = %.0f%% (refined: ambiguous names counted only "
              "when the base is a known host or group expression)"
              % (tin, tall, 100.0 * tin / tall if tall else 0))
        print("   without hub fields (>= %d references in the file; %d of the used fields "
              "dropped): fields %d, private %d, refs inside %d of %d = %.0f%%"
              % (HUB, len(used) - len(nh), len(nh),
                 sum(1 for f in nh if f in only_in), hin, hall,
                 100.0 * hin / hall if hall else 0))
        print("   raw name match, no refinement: inside %d of %d = %.0f%%"
              % (rin, rall, 100.0 * rin / rall if rall else 0))
        print("   only inside: %s" % " ".join(only_in))
        print("   field: inside/outside (* = ambiguous name, refined count)")
        for f in sorted(used, key=lambda x: (-used[x][0], x)):
            print("     %s%s %s: %d/%d" % ("*" if f in ambiguous else " ",
                                          "".join(s[4] if False else "" for s in fieldset[f]),
                                          f, used[f][0], used[f][1]))

        gused = {}
        for v, lns in gv_refs.items():
            ins = sum(1 for ln in lns if ln in inside_lines)
            if ins:
                gused[v] = (ins, len(lns) - ins)
        print("b2. file-scope variables used: %d; only inside: %d; refs inside %d of %d"
              % (len(gused), sum(1 for v in gused.values() if v[1] == 0),
                 sum(v[0] for v in gused.values()),
                 sum(v[0] + v[1] for v in gused.values())))
        print("     " + " ".join("%s:%d/%d" % (v, a, b) for v, (a, b) in
                                 sorted(gused.items(), key=lambda kv: (-kv[1][0], kv[0]))))

        # c. locks and atomics
        lk = Counter()
        lre = re.compile(r'(pthread_mutex_(?:try)?lock|pthread_rwlock_\w+|pthread_cond_(?:wait|timedwait)|__atomic_\w+)\s*\(\s*(&?[^,()]*(?:\([^)]*\))?[^,()]*)')
        for n in names:
            body = "\n".join(slines[defs[n][0] - 1:defs[n][1]])
            for m in lre.finditer(body):
                op = m.group(1)
                if op.startswith("__atomic_") or op.startswith("pthread_cond"):
                    lk[op] += 1
                else:
                    arg = re.sub(r'\s+', '', m.group(2)).lstrip("&")
                    tail = re.split(r'->|\.', arg)[-1]
                    lk["%s(%s)" % (op, tail)] += 1
        print("c. locks and atomics (%d total):" % sum(lk.values()))
        for k, v in sorted(lk.items(), key=lambda kv: (-kv[1], kv[0])):
            print("     %4d  %s" % (v, k))

        # d. outside callers
        callers = defaultdict(Counter)  # cand fn -> {outside fn: count}
        for n in names:
            for ln, is_call in uses.get(n, ()):
                o = owner.get(ln, "<file scope>")
                if o in cset:
                    continue
                callers[n][o + ("" if is_call else " [value]")] += 1
        distinct = {c for d in callers.values() for c in d}
        entry = [n for n in names if not defs[n][2]]
        xfile = {n: sum(1 for t in other_txt.values() if re.search(r'\b%s\b' % n, t))
                 for n in entry}
        print("d. outside callers: %d distinct callers; %d of %d functions have one; "
              "non-static entry points %d (named in other src files: %d)"
              % (len(distinct), len(callers), len(names), len(entry),
                 sum(1 for v in xfile.values() if v)))
        for n in names:
            if n in callers:
                tot = sum(callers[n].values())
                lst = ", ".join("%s x%d" % (c, k) for c, k in
                                sorted(callers[n].items(), key=lambda kv: (-kv[1], kv[0]))[:6])
                more = len(callers[n]) - 6
                first = min(ln for ln, _ in uses[n] if owner.get(ln, "<file scope>") not in cset)
                print("     %s (L%d): %d calls from %d: %s%s [first L%d]"
                      % (n, defs[n][0], tot, len(callers[n]), lst,
                         " +%d more" % more if more > 0 else "", first))
        if "--sites" in flags:
            for n in names:
                sites = sorted((ln, owner.get(ln, "<file scope>")) for ln, _ in uses.get(n, ())
                               if owner.get(ln, "<file scope>") not in cset)
                if sites:
                    print("     sites %s: %s" % (n, " ".join("%s:%d" % (o, ln) for ln, o in sites)))
        if "--field-sites" in flags:
            for f in sorted(used):
                if 0 < used[f][1] <= 12:
                    print("     field-sites %s: %s" % (f, " ".join(
                        "%s:%d" % (owner.get(ln, "<file scope>"), ln)
                        for ln in refs[f] if ln not in inside_lines)))
        print("   functions with no outside caller: %s"
              % " ".join(n for n in names if n not in callers))
        print("   non-static entry points named in other src files: %s"
              % (", ".join("%s(%d)" % (n, v) for n, v in xfile.items()) or "none"))

        # e. outside static callees
        callee = Counter()
        for n in names:
            for m in name_re.finditer("\n".join(slines[defs[n][0]:defs[n][1]])):
                x = m.group(1)
                if x in statics and x not in cset and x != n:
                    seg_start = "\n".join(slines[defs[n][0]:defs[n][1]])[m.end():].lstrip()
                    if seg_start.startswith("("):
                        callee[x] += 1
        print("e. outside static callees: %d distinct (%d calls)"
              % (len(callee), sum(callee.values())))
        print("     " + " ".join("%s(%d)" % (k, v) for k, v in
                                 sorted(callee.items(), key=lambda kv: (-kv[1], kv[0]))))

        # f. tests
        pertest = {n: sorted(os.path.relpath(p, root) for p, t in test_txt.items()
                             if re.search(r'\b%s\b' % n, t)) for n in names}
        files = {f for v in pertest.values() for f in v}
        none = [n for n in names if not pertest[n]]
        print("f. tests: %d files name at least one function; %d of %d functions "
              "are named by no test"
              % (len(files), len(none), len(names)))
        print("   no test names: %s" % " ".join(none))
        srct = sorted(f for f in files
                      if re.search(r'raft_host\.c|raft_host"', test_txt[os.path.join(root, f)]))
        print("   of those, %d files also read raft_host.c as text or include it "
              "(they break when a function moves): %s" % (len(srct), " ".join(srct)))

        hot_note = ""
        rows[key] = dict(gv=len(gused), gv_in=sum(1 for v in gused.values() if v[1] == 0),
                         n=len(names), lines=lines_tot, fields=len(used),
                         fields_in=len(only_in),
                         share=100.0 * tin / tall if tall else 0,
                         share_hub=100.0 * hin / hall if hall else 0, fields_nh=len(nh),
                         priv_nh=sum(1 for f in nh if f in only_in),
                         share_raw=100.0 * rin / rall if rall else 0,
                         callers=len(distinct), callers_fn=len(callers),
                         callees=len(callee), tests=len(files), notest=len(none),
                         locks=sum(v for k, v in lk.items() if "lock" in k),
                         atomics=sum(v for k, v in lk.items() if k.startswith("__atomic")))

    print("\n=== Summary ===")
    cols = [("n", "functions"), ("lines", "lines"), ("fields", "fields used"),
            ("fields_in", "only inside"), ("gv", "file-scope vars"),
            ("gv_in", "vars only inside"), ("share", "refs inside %"),
            ("share_raw", "refs inside % raw"),
            ("fields_nh", "fields w/o hubs"), ("priv_nh", "private w/o hubs"),
            ("share_hub", "refs inside % w/o hubs"),
            ("callers", "outside callers"), ("callers_fn", "fns with outside caller"),
            ("callees", "outside callees"), ("tests", "test files"),
            ("notest", "fns no test"), ("locks", "lock calls"), ("atomics", "atomics")]
    print("| candidate | " + " | ".join(c[1] for c in cols) + " |")
    print("|---|" + "---|" * len(cols))
    for k, r in rows.items():
        print("| %s | " % k + " | ".join(
            ("%.0f" % r[c] if isinstance(r[c], float) else str(r[c])) for c, _ in cols) + " |")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
