#!/usr/bin/env python3
"""Reproducible directed EFS defect-navigation graph; no LLM/API/network.

Run with the Python environment that owns graphifyy (see tools/graphify/README.md).
Only literal document references and curated oracle links supplement the AST.
No automatic lock, happens-before, durability or test-coverage inference.
"""
from __future__ import annotations
import argparse
import contextlib
import io
from collections import Counter, defaultdict
import hashlib
import importlib.metadata
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def select_files(root, config):
    # Tracked source only: never scan another checkout, raw artifacts or symlinks.
    tracked = subprocess.check_output(["git", "ls-files", "-z"], cwd=root).decode().split("\0")
    code, docs = [], []
    for name in sorted(set(tracked) - {""} - set(config["exclude"])):
        p = root / name
        if p.is_symlink() or not p.is_file():
            continue
        first = name.split("/")[0]
        if first in config["code_roots"] and p.suffix in config["code_suffixes"]:
            code.append(name)
        elif name in config["documents"] or (p.suffix == ".md" and any(
                name.startswith(d + "/") for d in config["document_roots"])):
            docs.append(name)
    missing = set(config["documents"] + config["build_files"]) - set(tracked)
    if missing:
        raise ValueError(f"configured files not tracked: {sorted(missing)}")
    return code, docs


def file_id(name):
    return "efs:file:" + name


def component_tag(path):
    """src/client/efs_fuse/open.c -> client/efs_fuse.open; src/client/write.c -> client/write."""
    parts = Path(path).with_suffix("").parts[1:]
    return "/".join(parts[:-1]) + ("." if len(parts) > 2 else "/") + parts[-1] if len(parts) > 1 else parts[0]


def tests_for_module(root):
    spec = importlib.util.spec_from_file_location("tests_for", root / "scripts/tests_for.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_files(command, code, config):
    """The tracked test files a catalogue command names."""
    out = []
    for name in re.findall(r"(?:tests|scripts)/[\w./-]+", command):
        out += [name] if name in code else [name + ext for ext in config["code_suffixes"] if name + ext in code]
    return out


def components(root, config, code, docs):
    """Component nodes (Task EB): one per catalogue tag, declared prefix tag and
    source file in a guided directory (one with a README). Each names its guide,
    contract (.h), implementation (.c) and the test files scripts/tests_for.py
    selects for the tag (Task EF): its own rows EXTRACTED, a parent's rows
    INFERRED and labelled with the parent, none where tests_for refuses."""
    out = {}
    def entry(tag):
        if tag not in out:
            base = "src/" + tag.replace(".", "/")
            d = base if base in dirs else str(Path(base).parent)
            while d != "src" and d + "/README.md" not in docs:
                d = str(Path(d).parent)
            out[tag] = {"guide": d + "/README.md" if d != "src" else None,
                        "contract": base + ".h" if base + ".h" in code else None,
                        "impl": base + ".c" if base + ".c" in code else None, "tests": []}
        return out[tag]
    dirs = {str(Path(p).parent) for p in code}
    for path in code:
        if path.startswith("src/") and path.endswith(".c") and str(Path(path).parent) + "/README.md" in docs:
            entry(component_tag(path))
    tf = tests_for_module(root)
    catalogue = root / config["catalogue"]
    rows = list(tf.catalogue(catalogue))
    prefixes = tf.prefix_tags(catalogue)
    for tag in prefixes | {t for _, _, tags in rows for t in tags}:
        entry(tag)
    line_of = {}  # the catalogue line a command's row starts on, for the edge location
    for line, text in enumerate(catalogue.read_text().splitlines(), 1):
        line_of.setdefault(text.split("\t", 1)[0], line)
    for tag, e in out.items():
        try:
            selected, answered = tf.resolve(rows, tag, src=root / "src", prefixes=prefixes)
        except LookupError:
            continue
        method, confidence = ("test-catalogue", "EXTRACTED") if answered == tag else ("inherited:" + answered, "INFERRED")
        seen = set()
        for cmd, _ in selected:
            for t in test_files(cmd, code, config):
                if t not in seen:
                    seen.add(t)
                    e["tests"].append((t, line_of[cmd], method, confidence))
    return out


def section_id(name, line):
    return f"efs:section:{name}:{line}"


def document_sections(text):
    # Ignore headings inside fenced examples. Keep line numbers exact.
    out, fence = [], None
    for line, value in enumerate(text.splitlines(), 1):
        stripped = value.lstrip()
        if stripped.startswith(("```", "~~~")):
            marker = stripped[:3]
            fence = None if fence == marker else (marker if fence is None else fence)
            continue
        m = re.match(r"^(#{1,6})\s+(.+?)\s*#*\s*$", value)
        if m and fence is None:
            out.append((line, m.group(2), len(m.group(1))))
    return out


def resolve_symbol(nodes, descriptor):
    matches = [n for n in nodes.values() if n.get("source_file") == descriptor["file"]
               and n.get("label", "").removesuffix("()") == descriptor["symbol"]]
    if len(matches) != 1:
        raise ValueError(f"symbol must resolve uniquely: {descriptor}, {len(matches)} matches")
    return matches[0]["id"]


def validate(graph, root, cases):
    nodes = {n["id"]: n for n in graph["nodes"]}
    links = graph["links"]
    if len(nodes) != len(graph["nodes"]) or not graph["directed"]:
        raise ValueError("duplicate nodes or undirected graph")
    for edge in links:
        if edge["source"] not in nodes or edge["target"] not in nodes:
            raise ValueError(f"dangling endpoint: {edge}")
        if edge.get("confidence") not in ("EXTRACTED", "INFERRED", "AMBIGUOUS"):
            raise ValueError("edge lacks confidence provenance")
    def has(a, b, relation, method=None):
        return any(e["source"] == a and e["target"] == b and e.get("relation") == relation
                   and (method is None or e.get("evidence_method") == method) for e in links)
    verdicts = []
    for case in cases:
        symbol = resolve_symbol(nodes, case["production"])
        oracle = file_id(case["oracle"])
        bug = "efs:bug:" + case["id"]
        sections = [n["id"] for n in nodes.values() if n.get("source_file") == case["contract"]
                    and n.get("label") == case["heading"]]
        if len(sections) != 1 or not has(sections[0], bug, "documents_finding"):
            raise ValueError(f"missing contract: {case['id']}")
        if not has(bug, symbol, "affects") or not has(oracle, bug, "regression_for"):
            raise ValueError(f"missing issue/oracle trace: {case['id']}")
        if not has(oracle, symbol, "exercises"):
            raise ValueError(f"missing curated exercises link: {case['id']}")
        text = (root / case["oracle"]).read_text()
        for literal in case["oracle_literals"]:
            if literal not in text:
                raise ValueError(f"oracle changed; re-review required: {case['id']}: {literal}")
        for target in case["calls"]:
            dest = resolve_symbol(nodes, target)
            if not has(symbol, dest, "calls", "graphify-ast"):
                raise ValueError(f"missing AST call: {case['id']}: {target}")
        if case.get("oracle_caller"):
            caller = resolve_symbol(nodes, case["oracle_caller"])
            if not has(caller, symbol, "calls", "graphify-ast"):
                raise ValueError(f"missing oracle AST call: {case['id']}")
        verdicts.append({"case": case["id"], "navigation": "PASS", "limit": case["limit"]})
    return verdicts


def build(root, out):
    from graphify.extract import extract
    import networkx as nx
    from graphify.cluster import cluster
    config = json.loads((root / "tools/graphify/scope.json").read_text())
    cases = json.loads((root / "tools/graphify/known-bugs.json").read_text())["cases"]
    version = importlib.metadata.version("graphifyy")
    if version != config["graphify_version"]:
        raise ValueError(f"expected graphifyy {config['graphify_version']}, got {version}; revalidate before upgrading")
    code, docs = select_files(root, config)
    inputs = sorted(set(code + docs + config["build_files"] + [config["catalogue"],
        "tools/graphify/scope.json", "tools/graphify/known-bugs.json", "scripts/build-code-graph.py", "scripts/graph-community-labels.py"]))
    before = {p: digest(root / p) for p in inputs}
    print(f"Scope: {len(code)} code, {len(docs)} documents, {len(config['build_files'])} build files", flush=True)
    # Fresh extraction, bounded workers, no reuse of potentially stale semantic cache.
    with tempfile.TemporaryDirectory(prefix="efs-graph-extract-") as cache:
        capture = io.StringIO()
        with contextlib.redirect_stdout(capture), contextlib.redirect_stderr(capture):
            ast = extract([root / p for p in code], root=root, cache_root=Path(cache), max_workers=4)
        print(capture.getvalue(), end="", flush=True)
    extraction_warnings = [line.strip() for line in capture.getvalue().splitlines() if "warning:" in line]
    if ast.get("failed_sources"):
        raise ValueError(f"source extraction failed: {ast['failed_sources']}")
    nodes = {n["id"]: dict(n) for n in ast["nodes"]}
    edges = [dict(e) for e in ast["edges"]]
    for n in nodes.values():
        if n.get("source_file"):
            n["source_file"] = str(Path(n["source_file"]).relative_to(root)) if Path(n["source_file"]).is_absolute() else n["source_file"]
    def node(nid, label, source, line=1, kind="file"):
        nodes[nid] = {"id": nid, "label": label, "source_file": source, "source_location": f"L{line}",
                      "kind": kind, "_origin": "semantic", "extraction": "deterministic-source-index"}
    def edge(a, b, relation, source, line=1, method="literal", confidence="EXTRACTED"):
        edges.append({"source": a, "target": b, "relation": relation, "confidence": confidence,
                      "source_file": source, "source_location": f"L{line}", "evidence_method": method})
    for p in code + docs + config["build_files"]:
        node(file_id(p), p, p)
    for n in list(nodes.values()):
        sf = n.get("source_file")
        if sf in code and n["id"] != file_id(sf):
            edge(file_id(sf), n["id"], "contains", sf)
    sections = {}
    symbols = defaultdict(list)
    for n in nodes.values():
        if n.get("_callable") and n.get("source_file", "").startswith(("src/", "include/")):
            symbols[n.get("label", "").removesuffix("()")].append(n["id"])
    for p in docs:
        text = (root / p).read_text()
        starts = document_sections(text)
        for i, (line, label, level) in enumerate(starts):
            sid = section_id(p, line)
            node(sid, label, p, line, "document_section")
            sections[(p, label)] = sid
            edge(file_id(p), sid, "contains", p, line)
            end = starts[i + 1][0] - 1 if i + 1 < len(starts) else len(text.splitlines())
            for offset, value in enumerate(text.splitlines()[line - 1:end], line):
                # A literal mention is NEVER a call, a requirement or proof.
                for token in set(re.findall(r"\b[A-Za-z_]\w*\b", value)):
                    choices = symbols.get(token, [])
                    # Prefer the one implementation over header declarations.
                    impl = [nid for nid in choices if nodes[nid].get("source_file", "").startswith("src/")
                            and nodes[nid].get("source_file", "").endswith(".c")]
                    choices = impl if len(impl) == 1 else choices
                    if len(choices) == 1:
                        edge(sid, choices[0], "mentions_symbol", p, offset)
    for case in cases:
        sid = sections[(case["contract"], case["heading"])]
        prod = resolve_symbol(nodes, case["production"])
        bug = "efs:bug:" + case["id"]
        loc = int(nodes[sid]["source_location"][1:])
        node(bug, case["id"], case["contract"], loc, "known_finding")
        edge(sid, bug, "documents_finding", case["contract"], loc, "reviewed-known-bug-manifest")
        edge(bug, prod, "affects", "tools/graphify/known-bugs.json", method="reviewed-known-bug-manifest")
        edge(file_id(case["oracle"]), bug, "regression_for", case["oracle"], method="reviewed-known-bug-manifest")
        edge(file_id(case["oracle"]), prod, "exercises", case["oracle"], method="reviewed-known-bug-manifest")
    # Build definitions reference tracked source/test paths literally. No inference
    # that a mentioned binary executes on every platform or under every flag.
    for p in config["build_files"]:
        for line, text in enumerate((root / p).read_text().splitlines(), 1):
            for name in re.findall(r"(?:src|include|tests|scripts)/[\w./-]+", text):
                candidates = [name] if name in code else [name + ext for ext in config["code_suffixes"] if name + ext in code]
                for dest in candidates:
                    edge(file_id(p), file_id(dest), "references_path", p, line)
    # Component nodes (Task EB): guide, contract, implementation and the
    # catalogued tests of one component, so `graphify explain <tag>` is the
    # compact starting set. Layout and tests_for.py's answer; a parent's rows
    # are INFERRED and carry the parent tag.
    for tag, c in sorted(components(root, config, code, docs).items()):
        cid = "efs:component:" + tag
        node(cid, tag, c["impl"] or c["guide"] or config["catalogue"], 1, "component")
        if c["guide"]:
            edge(cid, file_id(c["guide"]), "guided_by", c["guide"], method="source-layout")
        if c["contract"]:
            edge(cid, file_id(c["contract"]), "contract", c["contract"], method="source-layout")
        if c["impl"]:
            edge(cid, file_id(c["impl"]), "implemented_by", c["impl"], method="source-layout")
        for test, line, method, confidence in c["tests"]:
            edge(cid, file_id(test), "tested_by", config["catalogue"], line, method, confidence)
    # Resolve quoted C includes by exact path, not matching a basename to a
    # possibly unrelated file. Unresolved imports remain labelled boundaries.
    for e in edges:
        if e.get("relation") not in ("imports", "imports_from") or e.get("target") in nodes:
            continue
        raw = e.get("metadata", {}).get("raw_include", "")
        candidates = ["include/" + raw, str(Path(e.get("source_file", "")).parent / raw)] if raw else []
        matches = sorted(set(p for p in candidates if p in code))
        if len(matches) == 1:
            e["target"] = file_id(matches[0])
            e["evidence_method"] = "exact-include-path"
        elif e.get("source") in nodes:
            label = raw or str(e["target"])
            boundary = "efs:import-boundary:" + label
            nodes.setdefault(boundary, {"id": boundary, "label": label,
                "kind": "unresolved_import_boundary", "source_file": "",
                "resolution": "May be external or unresolved project-local; no implementation claim"})
            e["target"] = boundary
    # External imports get explicitly marked boundary nodes; unresolved non-import
    # targets are reported and omitted, never invented as resolved calls.
    omitted = Counter()
    normalized = []
    seen = set()
    for e in edges:
        if e.get("source") not in nodes or e.get("target") not in nodes:
            omitted[e.get("relation", "unknown")] += 1
            continue
        e.setdefault("confidence", "EXTRACTED")
        e.setdefault("evidence_method", "graphify-ast")
        src = nodes[e["source"]]
        e.setdefault("source_file", src.get("source_file", ""))
        e.setdefault("source_location", src.get("source_location"))
        key = json.dumps(e, sort_keys=True)
        if key not in seen:
            seen.add(key)
            normalized.append(e)
    # MultiDiGraph preserves e.g. calls AND references between the same pair.
    graph = {"directed": True, "multigraph": True, "nodes": list(nodes.values()), "links": normalized}
    verdicts = validate(graph, root, cases)
    projection = nx.Graph()
    projection.add_nodes_from(nodes)
    projection.add_edges_from((e["source"], e["target"]) for e in normalized)
    communities = cluster(projection)
    spec = importlib.util.spec_from_file_location("community_labels", ROOT / "scripts/graph-community-labels.py")
    naming = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(naming)
    prior = json.loads((out / "graph.json").read_text())["nodes"] if (out / "graph.json").exists() else []
    descriptions = naming.describe(nodes, communities, normalized, prior)
    for cid, members in communities.items():
        for nid in members:
            nodes[nid].update(community=cid, community_name=descriptions[cid]["label"], community_label_version=1)
    # No write occurs until source stability and known-path validation succeed.
    after = {p: digest(root / p) for p in inputs}
    if before != after:
        raise ValueError("inputs changed during extraction; graph not published")
    symbol_counts = Counter(n.get("source_file", "") for n in ast["nodes"]
                            if n.get("label") != Path(n.get("source_file", "")).name)
    self_calls = sum(e["source"] == e["target"] and e["relation"] == "calls" for e in normalized)
    manifest = {"schema": 1, "self_call_edges": self_calls, "ast_symbol_counts": {p: symbol_counts[p] for p in code}, "source_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root).decode().strip(),
                "input_sha256": before, "graphify_version": version,
                "extractor_sha256": digest(Path(__import__("graphify.extract", fromlist=["extract"]).__file__)),
                "input_tokens": 0, "output_tokens": 0, "extraction_warnings": extraction_warnings, "omitted_unresolved_edges": dict(omitted)}
    graph["graph"] = {"efs_build": {k: v for k, v in manifest.items() if k not in ("input_sha256", "ast_symbol_counts")}}
    graph["nodes"] = sorted(nodes.values(), key=lambda n: n["id"])
    graph["links"] = sorted(normalized, key=lambda e: (e["source"], e["target"], e.get("relation", ""), json.dumps(e, sort_keys=True)))
    out.mkdir(parents=True, exist_ok=True)
    for name, data in [("graph.json", graph), ("source-manifest.json", manifest), ("validation.json", verdicts)]:
        temp = out / (name + ".tmp")
        temp.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
        temp.replace(out / name)
    from graphify.export import to_html
    display = nx.MultiDiGraph()
    display.add_nodes_from((n["id"], n) for n in graph["nodes"])
    for e in graph["links"]:
        display.add_edge(e["source"], e["target"], **{k: v for k, v in e.items() if k not in ("source", "target")})
    print("Visualization exceeds 5,000 nodes; exporting community overview, not the full node layout.", flush=True)
    if not to_html(display, communities, str(out / "graph.html"),
                   community_labels={cid: nodes[members[0]]["community_name"] for cid, members in communities.items()}, node_limit=5000):
        raise ValueError("community HTML export failed")
    naming.enrich_html(out / "graph.html", descriptions)
    relations = Counter(e["relation"] for e in normalized)
    report = ["# EFS defect-navigation graph", "", f"Source base: `{manifest['source_commit']}`; exact inputs: `source-manifest.json`.",
              f"Graphify {version}; {len(nodes):,} nodes, {len(normalized):,} directed typed edges, {len(communities)} communities.",
              "Parallel edges retained. Community clustering uses an undirected projection only; stored edges remain directed.",
              "Extraction cost: 0 input / 0 output LLM tokens (local structural extraction).", "", "## Known-bug navigation checks", ""]
    report += [f"- {v['case']}: {v['navigation']} — {v['limit']}" for v in verdicts]
    report += ["", "## Relationships", ""] + [f"- `{r}`: {n}" for r, n in sorted(relations.items())]
    report += ["", "## Extraction warnings", ""] + ["- " + warning for warning in extraction_warnings]
    report += ["", "## Limits", "", f"Health warning: {self_calls} self-call edges retained. These can be recursion or ambiguous callback resolution; inspect source before interpreting them.", f"Unresolved edges omitted: {dict(omitted)}. These are extraction gaps, not absent dependencies.",
               "Literal mentions and reviewed exercises links are not calls or runtime coverage. AST edges retain their confidence labels.",
               "Component nodes come from the source layout and scripts/tests_for.py's answer over the test catalogue (tests/budgets.tsv): tested_by is a catalogue route, not measured coverage; a route inherited from the parent tag is INFERRED with evidence_method inherited:<parent>.",
               "No automatic lock ownership, shared-state access, persistence-order, happens-before, or exhaustive build-variant analysis.",
               "Known-bug checks validate navigation, not a rerun of product acceptance or independent rediscovery of the bug.",
               "External library internals are excluded. Function pointers, macros and embedded C/shell may be unresolved."]
    (out / "GRAPH_REPORT.md").write_text("\n".join(report) + "\n")
    print(f"PASS: {len(nodes)} nodes, {len(normalized)} edges; {len(verdicts)} known-bug navigation checks; unresolved {dict(omitted)}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "graphify-out")
    parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args()
    if args.validate_only:
        cases = json.loads((ROOT / "tools/graphify/known-bugs.json").read_text())["cases"]
        manifest = json.loads((args.out / "source-manifest.json").read_text())
        config = json.loads((ROOT / "tools/graphify/scope.json").read_text())
        code, docs = select_files(ROOT, config)
        expected = set(code + docs + config["build_files"] + [config["catalogue"],
            "tools/graphify/scope.json", "tools/graphify/known-bugs.json", "scripts/build-code-graph.py", "scripts/graph-community-labels.py"])
        if expected != set(manifest["input_sha256"]) or any(
                digest(ROOT / p) != h for p, h in manifest["input_sha256"].items()):
            raise ValueError("graph is stale: source inputs differ; rebuild required")
        print(json.dumps(validate(json.loads((args.out / "graph.json").read_text()), ROOT, cases), indent=2))
    else:
        build(ROOT, args.out)


if __name__ == "__main__":
    main()
