#!/usr/bin/env python3
"""Summarize complete traced PUT sends by immutable ticket and fragment.

Diagnostic only. Truncated payloads are never checked as data; the first 64
header bytes suffice for identity. Unfinished/partial writes are counted but
never promoted to complete sends. No durability verdict follows from sends.
"""
import argparse
import ast
import collections
import json
from pathlib import Path
import re
import statistics

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('work', type=Path)
parser.add_argument('--ino', type=int, required=True)
args = parser.parse_args()
groups = collections.defaultdict(list)
incomplete = 0
for path in args.work.glob('client.trace.*'):
    for line in path.read_text(errors='replace').splitlines():
        if 'writev(' not in line or 'TCP:' not in line:
            continue
        quoted = re.findall(r'iov_base=("(?:[^"\\]|\\.)*")', line)
        if len(quoted) < 2:
            continue
        header, body = [ast.literal_eval('b' + x) for x in quoted[:2]]
        if len(header) != 5 or header[4] != 6 or len(body) < 64:
            continue
        ino = int.from_bytes(body[36:44], 'big')
        if ino != args.ino:
            continue
        size = re.search(r'\) = (\d+) <([0-9.]+)>$', line)
        if not size or int(size[1]) != int.from_bytes(header[:4], 'big') + 4:
            incomplete += 1
            continue
        peer = re.search(r'->127\.0\.0\.1:(\d+)', line)
        key = (int.from_bytes(body[28:36], 'big'), int.from_bytes(body[56:64], 'big'),
               int.from_bytes(body[4:8], 'big'), int(peer[1]) if peer else None)
        groups[key].append(float(line.split()[0]))
output = []
for (seq, obj, frag, port), times in sorted(groups.items()):
    times.sort()
    gaps = [b-a for a, b in zip(times, times[1:])]
    output.append(dict(seq=seq, object=hex(obj), fragment=frag, port=port,
                       complete_sends=len(times), first=times[0], last=times[-1],
                       gap_median_ms=1000*statistics.median(gaps) if gaps else None))
refusals = []
for path in (args.work/'cluster').glob('n*.log'):
    counts = collections.Counter()
    for line in path.read_text(errors='replace').splitlines():
        match = re.search(r'put-upload refused fi=(\d+) seq=(\d+) rc=(-?\d+)', line)
        if match:
            counts[tuple(map(int, match.groups()))] += 1
    for row in output:
        for (frag, seq, rc), count in counts.items():
            if frag == row['fragment'] and seq == row['seq']:
                refusals.append(dict(node_log=path.name, fragment=frag, seq=seq, rc=rc, count=count))
print(json.dumps(dict(ino=args.ino, uploads=output, incomplete_writes=incomplete,
                      server_refusals=refusals, limitation='send observations are not acknowledgements or durability'), indent=2))
