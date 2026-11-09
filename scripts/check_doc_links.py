#!/usr/bin/env python3
"""Docs gate: every relative Markdown link under docs/ and the repo root resolves.

Checks `[text](path)` and `[text](path#anchor)` (file existence only; anchors
are not verified). http(s)/mailto/other-scheme links and anchor-only links are
ignored, as are fenced code blocks and inline code spans. Exit 1 lists each
dangling link as file:line.

Evidence scripts: every tracked file under scripts/evidence/ must be named by
its full repo path (a link or a backticked path, fenced blocks included) in at
least one tracked docs/ page; a bare basename does not count, so an inventory
page listing basenames cites nothing. Exit 1 lists each orphan script.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LINK = re.compile(r"\]\(\s*<?([^)\s>]+)")
REF = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*<?(\S+?)>?(?:\s|$)")
CODE = re.compile(r"`[^`]*`")


def md_files():
    yield from sorted(ROOT.glob("*.md"))
    yield from sorted((ROOT / "docs").rglob("*.md"))
    yield from sorted((ROOT / "src").rglob("README.md"))


def dangling(path):
    fence = None
    for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        m = re.match(r"\s*(`{3,}|~{3,})", line)
        # A closing fence is the same character, at least as long as the opener.
        if m and (fence is None or (m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence))):
            fence = None if fence else m.group(1)
            continue
        if fence:
            continue
        line = CODE.sub("", line)
        targets = LINK.findall(line)
        r = REF.match(line)
        if r:
            targets.append(r.group(1))
        for t in targets:
            if re.match(r"[A-Za-z][A-Za-z0-9+.-]*:", t) or t.startswith("#"):
                continue
            rel = t.split("#", 1)[0].split("?", 1)[0]
            if not rel:
                continue
            base = ROOT if rel.startswith("/") else path.parent
            if not (base / rel.lstrip("/")).exists():
                yield n, t


def git_files(*spec):
    out = subprocess.run(["git", "ls-files", "-z", "--", *spec], cwd=ROOT,
                         capture_output=True, text=True, check=True).stdout
    return [f for f in out.split("\0") if f]


def orphan_scripts():
    cited = set()
    for page in git_files("docs/*.md"):
        cited.update(re.findall(r"scripts/evidence/[\w./+-]+", (ROOT / page).read_text(errors="replace")))
    return [f for f in git_files("scripts/evidence") if f not in cited]


def main():
    bad = [(p, n, t) for p in md_files() for n, t in dangling(p)]
    for p, n, t in bad:
        print(f"{p.relative_to(ROOT)}:{n}: dangling link {t}")
    orphans = orphan_scripts()
    for f in orphans:
        print(f"{f}: orphan script (no docs/ page names it)")
    print(f"check_doc_links: {len(bad)} dangling link(s), {len(orphans)} orphan script(s)")
    return 1 if bad or orphans else 0


if __name__ == "__main__":
    sys.exit(main())
