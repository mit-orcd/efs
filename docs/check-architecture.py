#!/usr/bin/env python3
"""docs/check-architecture.py — the machine gate for the architecture docs.

Implements the documentation gate specified in docs/arch/development.md
("The specification itself is under a machine gate"):

  1. regen-diff          regenerate docs/architecture-full.md (to a temp
                         file — the checked-in file is never clobbered) and
                         fail on any diff.
  2. links               every internal markdown link in the doc sources
                         resolves (target file exists; #anchor, when present,
                         names a header in the target).
  3. invariant-refs      every invariant reference (I1..I25, incl. ranges
                         like I20–I23 / I1..I25) names an invariant defined
                         in architecture.md §4.
  4. op-matrix-sections  every §-reference inside the §6 operation matrix
                         names an existing numbered section of
                         architecture.md.
  5. single-home         no normative table (same header row), no invariant
                         definition, and no placement-formula definition is
                         stated in more than one place. Satellites explain,
                         they never restate; arch/START-HERE.md links rather
                         than restates; architecture.html is a rendition,
                         not an md source.

Scope: the normative index (docs/architecture.md) plus the satellites under
docs/arch/. The generated docs/architecture-full.md is covered by check 1
and excluded from the content checks. Exit 0 on pass, 1 with per-check
diagnostics on failure. Python 3 standard library only; CWD-independent.

    python3 docs/check-architecture.py
"""
import contextlib
import importlib.util
import io
import pathlib
import re
import sys
import tempfile

DOCS = pathlib.Path(__file__).resolve().parent
INDEX = DOCS / "architecture.md"
FULL = DOCS / "architecture-full.md"
GEN = DOCS / "gen-architecture-full.py"


def doc_sources():
    """The index + every satellite. The generated full file is NOT a source."""
    return [INDEX] + sorted((DOCS / "arch").rglob("*.md"))


def rel(path):
    return path.relative_to(DOCS).as_posix()


def lines_without_fences(text):
    """Return the text's lines with fenced-code contents blanked (line
    numbers preserved), so example text inside ``` blocks is not scanned."""
    out = []
    in_fence = False
    for line in text.splitlines():
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            out.append("")
            continue
        out.append("" if in_fence else line)
    return out


# ---------------------------------------------------------------- check 1
def check_regen_diff():
    """Regenerate architecture-full.md to a temp path; fail on any diff."""
    if not FULL.exists():
        return [f"{rel(FULL)} is missing; run: python3 {rel(GEN)}"]
    spec = importlib.util.spec_from_file_location("gen_arch_full", GEN)
    gen = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gen)
    with tempfile.NamedTemporaryFile(
        "w", suffix=".md", dir=DOCS, delete=False
    ) as tmp:
        tmp_path = pathlib.Path(tmp.name)
    try:
        gen.OUT = tmp_path  # redirect the build artifact; never clobber FULL
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = gen.main()
        if rc != 0:
            return [f"generator {rel(GEN)} failed (rc={rc})"]
        regenerated = tmp_path.read_text()
    finally:
        try:
            tmp_path.unlink()
        except FileNotFoundError:
            pass
    current = FULL.read_text()
    if regenerated == current:
        return []
    old, new = current.splitlines(), regenerated.splitlines()
    first = next(
        (i + 1 for i, (a, b) in enumerate(zip(old, new)) if a != b),
        min(len(old), len(new)) + 1,
    )
    return [
        f"{rel(FULL)} is stale: first differs at line {first} "
        f"({len(old)} vs {len(new)} lines). "
        f"Regenerate with: python3 {rel(GEN)}"
    ]


# ---------------------------------------------------------------- check 2
LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+[^)]*)?\)")
HEADER_RE = re.compile(r"^#{1,6}\s+(.*?)\s*#*\s*$")


def github_anchors(text):
    """GitHub-style anchor slugs for every header, incl. -1/-2 dedup."""
    anchors = set()
    seen = {}
    for line in lines_without_fences(text):
        m = HEADER_RE.match(line)
        if not m:
            continue
        h = re.sub(r"[`*]", "", m.group(1))  # strip code/emphasis markers
        h = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", h)  # links -> their text
        h = re.sub(r"[^\w\s-]", "", h, flags=re.UNICODE).lower()
        slug = h.replace(" ", "-")
        n = seen.get(slug, 0)
        seen[slug] = n + 1
        anchors.add(slug if n == 0 else f"{slug}-{n}")
    return anchors


def check_links():
    problems = []
    anchor_cache = {}
    for src in doc_sources():
        lines = lines_without_fences(src.read_text())
        for lineno, line in enumerate(lines, 1):
            for m in LINK_RE.finditer(line):
                target = m.group(1)
                if re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target):
                    continue  # external (https:, mailto:, ...)
                file_part, _, anchor = target.partition("#")
                if file_part:
                    dest = (src.parent / file_part).resolve()
                    try:
                        dest.relative_to(DOCS.parent.resolve())
                    except ValueError:
                        problems.append(
                            f"{rel(src)}:{lineno}: link escapes the repo: {target}"
                        )
                        continue
                    if not dest.is_file():
                        problems.append(
                            f"{rel(src)}:{lineno}: link target does not exist: "
                            f"{target}"
                        )
                        continue
                else:
                    dest = src
                if anchor:
                    if dest not in anchor_cache:
                        anchor_cache[dest] = github_anchors(dest.read_text())
                    if anchor not in anchor_cache[dest]:
                        problems.append(
                            f"{rel(src)}:{lineno}: link anchor not found in "
                            f"{rel(dest)}: #{anchor}"
                        )
    return problems


# ---------------------------------------------------------------- check 3
I_REF_RE = re.compile(r"(?<![\w])I(\d+)(?![\w])")
I_RANGE_RE = re.compile(r"(?<![\w])I(\d+)\s*(?:–|—|\.\.|-)\s*I(\d+)(?![\w])")


def section_bounds(text, header_re):
    """(start, end) line offsets of the `## N.` section; None if absent."""
    lines = text.splitlines()
    start = None
    for i, line in enumerate(lines):
        if start is None:
            if header_re.match(line):
                start = i
        elif re.match(r"^## ", line):
            return start, i
    return (start, len(lines)) if start is not None else None


def defined_invariants(index_text):
    bounds = section_bounds(index_text, re.compile(r"^## 4\.\s"))
    if bounds is None:
        return None
    s, e = bounds
    return {int(n) for n in re.findall(r"\*\*I(\d+)", "\n".join(index_text.splitlines()[s:e]))}


def check_invariant_refs(defined):
    problems = []
    lo, hi = min(defined), max(defined)
    for src in doc_sources():
        lines = lines_without_fences(src.read_text())
        for lineno, line in enumerate(lines, 1):
            for m in I_RANGE_RE.finditer(line):
                a, b = int(m.group(1)), int(m.group(2))
                for n in range(min(a, b), max(a, b) + 1):
                    if n not in defined:
                        problems.append(
                            f"{rel(src)}:{lineno}: range I{a}–I{b} covers I{n}, "
                            f"which §4 does not define (defined: I{lo}..I{hi})"
                        )
            for m in I_REF_RE.finditer(line):
                n = int(m.group(1))
                if n not in defined:
                    problems.append(
                        f"{rel(src)}:{lineno}: references I{n}, which §4 does "
                        f"not define (defined: I{lo}..I{hi})"
                    )
    return problems


# ---------------------------------------------------------------- check 4
SECTION_REF_RE = re.compile(r"§\s*(\d+(?:\.\d+)?)")


def index_sections(index_text):
    """Numbered sections of the index: '2', '7.3', ..."""
    ids = set()
    for line in index_text.splitlines():
        m = re.match(r"^## (\d+)\.\s", line)
        if m:
            ids.add(m.group(1))
        m = re.match(r"^### (\d+\.\d+)\s", line)
        if m:
            ids.add(m.group(1))
    return ids


def check_op_matrix_sections(index_text, sections):
    problems = []
    bounds = section_bounds(index_text, re.compile(r"^## 6\.\s"))
    if bounds is None:
        return ["architecture.md: §6 operation matrix not found"]
    s, e = bounds
    for lineno, line in enumerate(index_text.splitlines()[s:e], s + 1):
        op = None
        if line.startswith("|") and not re.match(r"^\|[ :|-]+\|$", line):
            op = line.split("|")[1].strip()
            if op == "Op":
                continue
        for m in SECTION_REF_RE.finditer(line):
            ref = m.group(1)
            if ref not in sections:
                where = f"row '{op}'" if op else "prose"
                problems.append(
                    f"{rel(INDEX)}:{lineno}: §6 {where} references §{ref}, "
                    f"which is not a numbered section of architecture.md"
                )
    return problems


# ---------------------------------------------------------------- check 5
FORMULA_RE = re.compile(
    r"(?<![\w])"
    r"(lane_shard\([^)]*\)|lane\([^)]*\)|stride\([^)]*\)|inode_shard\([^)]*\)"
    r"|dentry_shard|dir_lane|chunk_meta_shard|size_lane)"
    r"\s*(?<![=!<>])=(?![=])\s*([^|`]+)"
)
INVARIANT_DEF_RE = re.compile(r"^\s*[-*]?\s*\*\*([IL])(\d+)\s*[·.]")


def norm_formula(lhs, rhs):
    lhs = re.sub(r"\s+", "", lhs)
    rhs = re.sub(r"\s+", " ", rhs).strip().rstrip(".,;")
    return f"{lhs} = {rhs}"


def check_single_home(index_text):
    problems = []

    # 5a — a normative table (its header row) may be defined in only one place.
    headers = {}  # normalized header -> [(file, lineno)]
    for src in doc_sources():
        prev = None
        for lineno, line in enumerate(src.read_text().splitlines(), 1):
            if re.match(r"^\|[ :|-]+\|$", line) and prev and prev.startswith("|"):
                cells = [re.sub(r"\s+", " ", c).strip() for c in prev.split("|")]
                key = "|".join(c for c in cells if c)
                headers.setdefault(key, []).append((src, lineno - 1))
            prev = line
    for key, locs in sorted(headers.items()):
        if len(locs) > 1:
            where = ", ".join(f"{rel(f)}:{n}" for f, n in locs)
            problems.append(
                f"normative table defined {len(locs)} times ({where}): | {key} |"
            )

    # 5b — the invariant list's only home is architecture.md §4.
    bounds = section_bounds(index_text, re.compile(r"^## 4\.\s"))
    s4 = set(range(*bounds)) if bounds else set()
    for src in doc_sources():
        for lineno, line in enumerate(src.read_text().splitlines(), 1):
            m = INVARIANT_DEF_RE.match(line)
            if not m:
                continue
            if src == INDEX and (lineno - 1) in s4:
                continue
            problems.append(
                f"{rel(src)}:{lineno}: defines invariant {m.group(1)}{m.group(2)} "
                f"outside its one home (architecture.md §4)"
            )

    # 5c — a placement formula may be defined in only one source.
    formulas = {}  # normalized formula -> [(file, lineno)]
    for src in doc_sources():
        for lineno, line in enumerate(src.read_text().splitlines(), 1):
            for m in FORMULA_RE.finditer(line):
                key = norm_formula(m.group(1), m.group(2))
                loc = (src, lineno)
                if loc not in formulas.setdefault(key, []):
                    formulas[key].append(loc)
    for key, locs in sorted(formulas.items()):
        files = {f for f, _ in locs}
        if len(files) > 1:
            where = ", ".join(f"{rel(f)}:{n}" for f, n in locs)
            problems.append(
                f"placement formula defined in {len(files)} sources ({where}): {key}"
            )
    return problems


# ------------------------------------------------------------------- main
def main():
    checks = []

    checks.append(("regen-diff: architecture-full.md is current", check_regen_diff()))

    checks.append(("links: every internal link resolves", check_links()))

    defined = defined_invariants(INDEX.read_text())
    if not defined:
        checks.append(("invariant-refs: I-references name §4 invariants",
                       ["architecture.md §4 defines no invariants — gate cannot run"]))
    else:
        checks.append(("invariant-refs: I-references name §4 invariants",
                       check_invariant_refs(defined)))

    sections = index_sections(INDEX.read_text())
    if not sections:
        checks.append(("op-matrix-sections: §6 names existing sections",
                       ["architecture.md has no numbered sections — gate cannot run"]))
    else:
        checks.append(("op-matrix-sections: §6 names existing sections",
                       check_op_matrix_sections(INDEX.read_text(), sections)))

    checks.append(("single-home: normative tables / invariants / formulas "
                   "defined once", check_single_home(INDEX.read_text())))

    failed = 0
    for name, problems in checks:
        if problems:
            failed += 1
            print(f"FAIL  {name}")
            for p in problems:
                print(f"      {p}")
        else:
            print(f"ok    {name}")
    if failed:
        print(f"\narchitecture doc gate: {failed}/{len(checks)} checks failed")
        return 1
    print(f"\narchitecture doc gate: all {len(checks)} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
