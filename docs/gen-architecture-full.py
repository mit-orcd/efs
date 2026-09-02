#!/usr/bin/env python3
"""Generate docs/architecture-full.md — the complete architecture as ONE file.

Concatenates the normative index (architecture.md) with every satellite
document under arch/, so the whole specification can be pasted into another
tool/agent in one shot. Sources of truth are the individual files; this file
is a build artifact. Run after any doc edit:

    python3 docs/gen-architecture-full.py
"""
import pathlib
import re
import sys

DOCS = pathlib.Path(__file__).resolve().parent
OUT = DOCS / "architecture-full.md"

# (source, appendix title) in reading order.
APPENDICES = [
    ("arch/START-HERE.md", "Start here — task routing for contributors"),
    ("arch/naming.md", "Naming"),
    ("arch/design.md", "Design rationale"),
    ("arch/failure-tolerance.md", "Failure tolerance — derivation"),
    ("arch/protocols/transactions.md", "Protocol — cross-shard transactions"),
    ("arch/protocols/data.md", "Protocol — data plane"),
    ("arch/protocols/directory.md", "Protocol — directory placement & spreading"),
    ("arch/protocols/sessions.md", "Protocol — sessions, open-unlinked, locking"),
    ("arch/performance.md", "Performance — multi-Raft runtime & hot-path contract"),
    ("arch/development.md", "Development — modularity constraint"),
    ("arch/verification.md", "Verification — simulator & code→signal cycle"),
    ("arch/design-history.md", "Design history (review rounds)"),
]

HEADER = """\
# efs architecture — full one-file rendition

**GENERATED FILE — do not edit.** Built by `docs/gen-architecture-full.py`
from `architecture.md` plus every satellite under `arch/`. Regenerate after
any doc edit:

```bash
python3 docs/gen-architecture-full.py
```

This is the **complete** architecture content in one paste: the normative
index first (it wins any disagreement), then each satellite verbatim as an
appendix. Relative links in the index (e.g. `arch/protocols/data.md`) refer
to the appendices below.

---
"""


def strip_and_demote(text: str) -> str:
    """Drop the satellite's H1 + nav-link block; demote all headers one level."""
    lines = text.splitlines()
    out = []
    i = 0
    # Skip the H1.
    if i < len(lines) and lines[i].startswith("# "):
        i += 1
    # Skip blank lines, then the nav block (consecutive lines starting with '[').
    while i < len(lines) and not lines[i].strip():
        i += 1
    while i < len(lines) and lines[i].lstrip().startswith("["):
        i += 1
    # Skip trailing blank lines after nav.
    while i < len(lines) and not lines[i].strip():
        i += 1
    for line in lines[i:]:
        out.append("#" + line if line.startswith("#") else line)
    body = "\n".join(out).strip()
    # Links back to the index point at its H1 anchor inside the combined file.
    body = re.sub(r"\]\((?:\.\./)+architecture\.md\)", "](#architecture)", body)
    return body


def main() -> int:
    index = (DOCS / "architecture.md").read_text()
    parts = [HEADER, index.strip(), "\n\n---\n\n# Appendices — satellite documents, verbatim\n"]
    for n, (rel, title) in enumerate(APPENDICES, 1):
        src = DOCS / rel
        if not src.exists():
            print(f"missing source: {rel}", file=sys.stderr)
            return 1
        body = strip_and_demote(src.read_text())
        parts.append(
            f"\n## Appendix {n} — {title}\n\n*Source: `{rel}` (headers demoted, nav stripped).*\n\n{body}\n"
        )
    OUT.write_text("\n".join(parts) + "\n")
    print(f"wrote {OUT} ({len(OUT.read_text().splitlines())} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
