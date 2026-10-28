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
    inputs = sorted(set(code + docs + config["build_files"] + [
        "tools/graphify/scope.json", "tools/graphify/known-bugs.json", "scripts/build-code-graph.py"]))
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
    for cid, members in communities.items():
        paths = Counter(nodes[n].get("source_file", "").rsplit("/", 1)[0] for n in members)
        label = paths.most_common(1)[0][0] or "External interfaces"
        for nid in members:
            nodes[nid].update(community=cid, community_name=label)
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
        expected = set(code + docs + config["build_files"] + [
            "tools/graphify/scope.json", "tools/graphify/known-bugs.json", "scripts/build-code-graph.py"])
        if expected != set(manifest["input_sha256"]) or any(
                digest(ROOT / p) != h for p, h in manifest["input_sha256"].items()):
            raise ValueError("graph is stale: source inputs differ; rebuild required")
        print(json.dumps(validate(json.loads((args.out / "graph.json").read_text()), ROOT, cases), indent=2))
    else:
        build(ROOT, args.out)


if __name__ == "__main__":
    main()
