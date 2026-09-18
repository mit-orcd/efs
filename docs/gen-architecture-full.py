#!/usr/bin/env python3
"""Generate the two build artifacts of the architecture spec:

  docs/architecture-full.md   the whole spec as ONE markdown file (for pasting
                              into another tool/agent)
  docs/architecture.html      the same content rendered for a browser, with a
                              table of contents

Both are built from the normative index (architecture.md) plus every
satellite under arch/. The markdown sources are the ONLY source of truth;
never edit either artifact by hand — `make docs-check` fails if they differ
from what this script produces. Run after any doc edit:

    python3 docs/gen-architecture-full.py
"""
import html as _html
import pathlib
import re
import sys

DOCS = pathlib.Path(__file__).resolve().parent
OUT = DOCS / "architecture-full.md"
OUT_HTML = DOCS / "architecture.html"

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


# ----------------------------------------------------------------------------
# Markdown -> HTML. Deliberately small: it renders exactly the constructs the
# spec uses (ATX headers, fenced code, pipe tables, blockquotes, nested -/1.
# lists, ---, paragraphs; inline code/bold/italic/links). Anchors use the same
# GitHub slug rule as check-architecture.py so in-page links resolve.
# ----------------------------------------------------------------------------

def _slug(text, seen):
    h = re.sub(r"[`*]", "", text)
    h = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", h)
    h = re.sub(r"[^\w\s-]", "", h, flags=re.UNICODE).lower()
    slug = h.replace(" ", "-")
    n = seen.get(slug, 0)
    seen[slug] = n + 1
    return slug if n == 0 else f"{slug}-{n}"


def _satellite_anchor_map(titles):
    """Map every satellite path (as written in links) to its appendix slug."""
    m = {}
    seen = {}
    for n, ((rel, _), title) in enumerate(zip(APPENDICES, titles), 1):
        slug = _slug(f"Appendix {n} — {title}", seen)
        for variant in (rel, "arch/" + rel.split("arch/", 1)[-1], rel.split("/")[-1]):
            m[variant] = slug
        # links from inside arch/ use relative forms: protocols/data.md, ../x.md
        tail = rel.split("arch/", 1)[-1]
        m[tail] = slug
        m["../" + tail] = slug
        if "/" in tail:
            m[tail.split("/", 1)[1]] = slug
    return m


_INLINE_CODE = re.compile(r"`([^`]+)`")
_BOLD = re.compile(r"\*\*(.+?)\*\*")
_ITAL = re.compile(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])|(?<!\w)_(?!\s)(.+?)(?<!\s)_(?!\w)")
_LINK = re.compile(r"\[([^\]]+)\]\(([^)\s]+)\)")


def _inline(text, anchors):
    # protect code spans first
    codes = []
    def keep(m):
        codes.append(_html.escape(m.group(1)))
        return f"\x00{len(codes)-1}\x00"
    text = _INLINE_CODE.sub(keep, text)
    text = _html.escape(text, quote=False)
    def link(m):
        label, href = m.group(1), m.group(2)
        if href.startswith("#"):
            pass
        elif href.startswith(("http://", "https://")):
            pass
        else:
            path, _, frag = href.partition("#")
            path = path.replace("../", "", 1) if path.startswith("../") and path.replace("../", "", 1) in anchors else path
            if frag:
                href = "#" + frag
            elif path in anchors:
                href = "#" + anchors[path]
            elif path.endswith("architecture.md"):
                href = "#efs-architecture--full-one-file-rendition"
            elif path.startswith("../"):
                # satellites live one level below docs/; the artifact is in docs/
                href = path[3:] + (("#" + frag) if frag else "")
            # else: leave as a relative file link
        return f'<a href="{_html.escape(href, quote=True)}">{label}</a>'
    text = _LINK.sub(link, text)
    text = _BOLD.sub(r"<strong>\1</strong>", text)
    text = _ITAL.sub(lambda m: f"<em>{m.group(1) or m.group(2)}</em>", text)
    return re.sub(r"\x00(\d+)\x00", lambda m: f"<code>{codes[int(m.group(1))]}</code>", text)


def _table(rows, anchors):
    cells = [[c.strip() for c in r.strip().strip("|").split("|")] for r in rows]
    if len(cells) < 2:
        return ""
    head, body = cells[0], cells[2:]
    out = ["<table><thead><tr>"]
    out += [f"<th>{_inline(c, anchors)}</th>" for c in head]
    out.append("</tr></thead><tbody>")
    for r in body:
        out.append("<tr>" + "".join(f"<td>{_inline(c, anchors)}</td>" for c in r) + "</tr>")
    out.append("</tbody></table>")
    return "".join(out)


_LIST_ITEM = re.compile(r"^(\s*)([-*]|\d+[.)])\s+(.*)$")


def _render_list(items, anchors):
    """items: list of (indent, ordered, text). Nested by indent."""
    out = []
    stack = []  # (indent, tag)
    for indent, ordered, text in items:
        tag = "ol" if ordered else "ul"
        while stack and stack[-1][0] > indent:
            out.append(f"</li></{stack.pop()[1]}>")
        if not stack or stack[-1][0] < indent:
            stack.append((indent, tag))
            out.append(f"<{tag}>")
        else:
            out.append("</li>")
        out.append(f"<li>{_inline(text, anchors)}")
    while stack:
        out.append(f"</li></{stack.pop()[1]}>")
    return "".join(out)


def _blocks(md, anchors):
    lines = md.splitlines()
    out, toc = [], []
    seen = {}
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line.strip():
            i += 1
            continue
        if line.lstrip().startswith("```"):
            j = i + 1
            while j < len(lines) and not lines[j].lstrip().startswith("```"):
                j += 1
            code = _html.escape("\n".join(lines[i + 1:j]))
            out.append(f"<pre><code>{code}</code></pre>")
            i = j + 1
            continue
        m = re.match(r"^(#{1,6})\s+(.*?)\s*#*\s*$", line)
        if m:
            lvl, text = len(m.group(1)), m.group(2)
            slug = _slug(text, seen)
            out.append(f'<h{lvl} id="{slug}">{_inline(text, anchors)}</h{lvl}>')
            if lvl <= 3:
                toc.append((lvl, slug, re.sub(r"[`*]", "", text)))
            i += 1
            continue
        if re.match(r"^-{3,}\s*$", line):
            out.append("<hr>")
            i += 1
            continue
        if line.startswith("|"):
            j = i
            while j < len(lines) and lines[j].startswith("|"):
                j += 1
            out.append(_table(lines[i:j], anchors))
            i = j
            continue
        if line.startswith(">"):
            j = i
            buf = []
            while j < len(lines) and lines[j].startswith(">"):
                buf.append(lines[j][1:].lstrip())
                j += 1
            inner, _ = _blocks("\n".join(buf), anchors)
            out.append(f"<blockquote>{inner}</blockquote>")
            i = j
            continue
        if _LIST_ITEM.match(line):
            items = []
            j = i
            while j < len(lines):
                lm = _LIST_ITEM.match(lines[j])
                if lm:
                    indent = len(lm.group(1))
                    items.append([indent, lm.group(2)[0].isdigit(), lm.group(3)])
                    j += 1
                    # continuation lines: indented, non-blank, not a new item
                    while j < len(lines) and lines[j].strip() and not _LIST_ITEM.match(lines[j]) \
                            and (len(lines[j]) - len(lines[j].lstrip())) > indent:
                        items[-1][2] += " " + lines[j].strip()
                        j += 1
                    continue
                break
            out.append(_render_list(items, anchors))
            i = j
            continue
        # paragraph
        j = i
        buf = []
        while j < len(lines) and lines[j].strip() and not lines[j].startswith(("|", ">", "#", "```")) \
                and not _LIST_ITEM.match(lines[j]) and not re.match(r"^-{3,}\s*$", lines[j]):
            buf.append(lines[j].strip())
            j += 1
        out.append(f"<p>{_inline(' '.join(buf), anchors)}</p>")
        i = j
    return "".join(out), toc


_CSS = """
:root{--bg:#0d1117;--panel:#161b22;--panel2:#1c2230;--line:#2b3444;--txt:#e6edf3;
--dim:#9aa7b8;--acc:#58a6ff;--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--txt);
font:16px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI",Helvetica,Arial,sans-serif}
.layout{display:grid;grid-template-columns:300px minmax(0,1fr);min-height:100vh}
nav{position:sticky;top:0;height:100vh;overflow-y:auto;padding:28px 18px;
background:var(--panel);border-right:1px solid var(--line);font-size:.86rem}
nav a{display:block;color:var(--dim);text-decoration:none;padding:.15em 0;line-height:1.35}
nav a:hover{color:var(--acc)}
nav .l1{color:var(--txt);font-weight:600;margin-top:1em}
nav .l2{padding-left:.9em}
nav .l3{padding-left:1.8em;font-size:.8rem}
main{max-width:980px;padding:48px 40px 120px}
h1{font-size:2.1rem;margin:.2em 0 .1em;letter-spacing:-.02em}
h2{font-size:1.45rem;margin:2.2em 0 .5em;padding-top:.6em;border-top:1px solid var(--line)}
h3{font-size:1.1rem;margin:1.6em 0 .3em;color:var(--acc)}
h4{font-size:1rem;margin:1.4em 0 .3em}
h5,h6{font-size:.95rem;margin:1.2em 0 .3em;color:var(--dim)}
p{margin:.55em 0}
code{font-family:var(--mono);font-size:.86em;background:#21262e;padding:.1em .38em;border-radius:5px}
pre{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:.8em 1em;overflow-x:auto}
pre code{background:none;padding:0;font-size:.84em}
a{color:var(--acc);text-decoration:none}
a:hover{text-decoration:underline}
table{border-collapse:collapse;width:100%;margin:1em 0;font-size:.92rem}
th,td{border:1px solid var(--line);padding:.45em .7em;text-align:left;vertical-align:top}
th{background:var(--panel2);font-weight:600}
td{background:var(--panel)}
blockquote{margin:.8em 0;padding:.4em 1em;border-left:3px solid var(--acc);
background:var(--panel);color:var(--dim);border-radius:0 6px 6px 0}
hr{border:0;border-top:1px solid var(--line);margin:2em 0}
ul,ol{padding-left:1.5em}
li{margin:.2em 0}
.gen{color:var(--dim);font-size:.85rem;border:1px dashed var(--line);padding:.6em .9em;border-radius:6px}
@media(max-width:900px){.layout{grid-template-columns:1fr}nav{position:static;height:auto}}
"""


def render_html(md, appendix_titles):
    anchors = _satellite_anchor_map(appendix_titles)
    body, toc = _blocks(md, anchors)
    nav = "".join(
        f'<a class="l{lvl}" href="#{slug}">{_html.escape(text)}</a>' for lvl, slug, text in toc
    )
    return (
        "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<title>efs — architecture</title>\n"
        "<!-- GENERATED by docs/gen-architecture-full.py from architecture.md + arch/*.md. Do not edit. -->\n"
        f"<style>{_CSS}</style>\n</head>\n<body>\n<div class=\"layout\">\n"
        f"<nav>{nav}</nav>\n<main>\n"
        "<p class=\"gen\">Generated view of <code>docs/architecture.md</code> and its "
        "satellites. The markdown is the source of truth; edit it, then run "
        "<code>make docs-check</code>.</p>\n"
        f"{body}\n</main>\n</div>\n</body>\n</html>\n"
    )


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
    combined = "\n".join(parts) + "\n"
    OUT.write_text(combined)
    print(f"wrote {OUT} ({len(combined.splitlines())} lines)")
    OUT_HTML.write_text(render_html(combined, [t for _, t in APPENDICES]))
    print(f"wrote {OUT_HTML} ({OUT_HTML.stat().st_size // 1024} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
