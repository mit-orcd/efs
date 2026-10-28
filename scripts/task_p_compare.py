#!/usr/bin/env python3
"""Compare Task P's closed gate results with Task A's corrected full runs.

Usage: task_p_compare.py ARTIFACT_ROOT
Prints JSON only. Missing/new gates have no fabricated baseline.
Times are scheduling evidence across different hardware/load, not throughput.
"""
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
text = (root/'docs/status/task-a-20261025.md').read_text()
section = text.split('## Corrected full runs and final F3 review', 1)[1].split('##',1)[0]
baseline = {}
for line in section.splitlines():
    fields = [v.strip() for v in line.split('|')]
    if len(fields) != 5 or not fields[2].startswith(('PASS / ', 'FAIL / ', 'NOTRUN / ')):
        continue
    baseline[fields[1]] = {n: float(fields[i].rsplit(' / ',1)[1]) for n,i in ((3,2),(5,3))}
art = Path(sys.argv[1])
report = {}
for n in (3,5):
    directory = art / f'loopback-{n}'
    rows=[]
    for f in sorted(directory.glob('gates/*/result.json')):
        d=json.loads(f.read_text());name=d.get('gate',f.parent.name)
        old=baseline.get(name,{}).get(n)
        # Task A spells the end-identity row with an underscore.
        if name=='_identity-end':old=baseline.get('_identity-end',{}).get(n)
        wall=d.get('seconds')
        rows.append(dict(gate=name,verdict=d.get('verdict'),reason=d.get('reason'),
                         seconds=wall,task_a_seconds=old,
                         ratio=round(wall/old,3) if old and wall is not None else None,
                         doubled=bool(old and wall is not None and wall>=2*old)))
    report[f'loopback-{n}']=rows
print(json.dumps(report,indent=2))
