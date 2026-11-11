#!/usr/bin/env python3
"""Position of the owner in `graphify query` output: python3 scripts/measure_question_routes.py [QUESTION COMPONENT]...
With no arguments, measures every question in tools/graphify/routes.json."""
import json, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def owner_labels(root, tag):
    """The component's label and its contract header path (the node labels that name the owner)."""
    g = json.loads((root / "graphify-out/graph.json").read_text())
    ids = {n["id"] for n in g["nodes"]}
    cid = "efs:component:" + tag
    if cid not in ids:
        raise SystemExit(f"unknown component {tag}")
    out = {tag}
    out.update(e["target"][len("efs:file:"):] for e in g["links"] if e["source"] == cid and e["relation"] == "contract")
    return out


def measure(root, question, tag, budget=None):
    cmd = ["graphify", "query", question] + (["--budget", str(budget)] if budget else [])
    text = subprocess.run(cmd, cwd=root, capture_output=True, text=True, check=True).stdout
    start = re.search(r"Start: \[(.*?)\] \|", text)
    shown = re.search(r"showing (\d+) of (\d+) nodes", text)
    found = re.search(r"(\d+) nodes found", text)
    labels = owner_labels(root, tag)
    lines = [l for l in text.splitlines() if l.startswith("NODE ")]
    labs = [re.match(r"NODE (.*?) \[src=", l).group(1) for l in lines]
    # a line names the owner by its label, or (a route node) by its owner attribute
    pos = next((i + 1 for i, (lab, l) in enumerate(zip(labs, lines)) if lab in labels or f'owner="{tag}"' in l), None)
    return {"start": start.group(1) if start else "", "position": pos, "shown": int(shown.group(1)) if shown else len(labs),
            "found": int(found.group(1)) if found else len(labs)}


if __name__ == "__main__":
    args = sys.argv[1:]
    pairs = list(zip(args[::2], args[1::2])) or [(r["question"], r["component"]) for r in json.loads((ROOT / "tools/graphify/routes.json").read_text())["routes"]]
    print("| question | owner | start nodes | owner position | nodes shown |\n|---|---|---|---|---|")
    for q, tag in pairs:
        r = measure(ROOT, q, tag)
        print(f"| {q} | {tag} | {r['start']} | {r['position'] or 'absent'} | {r['shown']} of {r['found']} |")
