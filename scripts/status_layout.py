#!/usr/bin/env python3
"""Move dated docs/status pages into docs/status/YYYY-MM/ and fix references.

A top-level page named `*-YYYYMMDD.md` or `*-YYYYMMDD-<suffix>.md` moves to
docs/status/YYYY-MM/<same name> with `git mv`. Undated pages and the living
documents stay put. Then every reference is rewritten: relative Markdown
links (recomputed from each file's old and new directory) and `docs/status/<name>`
path strings in any tracked text file. Generated files are skipped; regenerate
them. Idempotent: with nothing left to move it changes nothing.

Usage: python3 scripts/status_layout.py [--dry-run]
"""
import os
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STATUS = Path("docs/status")
LIVING = {"README.md", "alpha-1.0-2plus1-checklist.md", "stage1-acceptance-matrix-20261030.md"}
DATED = re.compile(r"-(\d{4})(0[1-9]|1[0-2])\d\d(-[A-Za-z0-9]+)?\.md$")
GENERATED = ("graphify-out/", "docs/how-it-works/architecture-full.md", "docs/how-it-works/architecture.html")
LINK = re.compile(r"(\]\(\s*<?|^\s{0,3}\[[^\]]+\]:\s*<?)([^)\s>]+)", re.M)


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True, text=True).stdout


def mapping():
    out = {}
    for p in sorted((ROOT / STATUS).glob("*.md")):
        m = DATED.search(p.name)
        if m and p.name not in LIVING:
            out[STATUS / p.name] = STATUS / f"{m.group(1)}-{m.group(2)}" / p.name
    return out


def rewrite_links(text, old_dir, new_dir, moved):
    def sub(m):
        t = m.group(2)
        if re.match(r"[A-Za-z][A-Za-z0-9+.-]*:", t) or t.startswith(("#", "/")):
            return m.group(0)
        rel, rest = re.match(r"([^?#]*)(.*)", t).groups()
        old = Path(os.path.normpath(old_dir / rel))
        new = moved.get(old, old)
        t2 = os.path.relpath(new, new_dir) + rest
        return m.group(1) + t2 if t2 != t else m.group(0)
    return LINK.sub(sub, text)


def main():
    dry = "--dry-run" in sys.argv
    moved = mapping()
    if not moved:
        print("status_layout: nothing to move")
        return 0
    counts = Counter(str(n.parent.name) for n in moved.values())
    for month, n in sorted(counts.items()):
        print(f"status_layout: {n} page(s) -> docs/status/{month}/")
    # Preflight before the first `git mv`: a failing move halfway through leaves
    # moved pages whose links a rerun no longer rewrites.
    tracked = set(git("ls-files", "--", str(STATUS)).splitlines())
    untracked = [str(o) for o in moved if str(o) not in tracked]
    if untracked:
        print("status_layout: untracked page(s), git add them first: " + " ".join(untracked), file=sys.stderr)
        return 1
    if dry:
        return 0
    for old, new in moved.items():
        (ROOT / new.parent).mkdir(exist_ok=True)
        git("mv", str(old), str(new))
    inv = {new: old for old, new in moved.items()}
    names = "|".join(re.escape(o.name) for o in sorted(moved, key=lambda p: -len(p.name)))
    # A path may wrap: `docs/status/` ends a comment line, the name starts the next.
    path_re = re.compile(rf"docs/status/((?:\n[ \t]*(?:\*|#|//)?[ \t]*)?)({names})(?![\w-])")
    for f in git("ls-files").splitlines():
        if f.startswith(GENERATED):
            continue
        p = ROOT / f
        try:
            text = p.read_text()
        except (UnicodeDecodeError, OSError):
            continue
        new_dir = Path(f).parent
        old_dir = inv[Path(f)].parent if Path(f) in inv else new_dir
        out = text
        if f.endswith(".md"):
            out = rewrite_links(out, old_dir, new_dir, moved)
        out = path_re.sub(lambda m: f"docs/status/{moved[STATUS / m.group(2)].parent.name}/{m.group(1)}{m.group(2)}", out)
        if out != text:
            p.write_text(out)
            if not f.startswith("docs/"):
                for n, (a, b) in enumerate(zip(text.splitlines(), out.splitlines()), 1):
                    if a != b:
                        print(f"rewrote {f}:{n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
