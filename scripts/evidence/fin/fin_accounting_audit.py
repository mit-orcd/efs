#!/usr/bin/env python3
"""Strict closed-fixture TCP/RDMA audit. Never edits counters or fixtures."""
import json
from pathlib import Path
import re
import shutil
import sys

art = Path(sys.argv[1]).resolve()
report, failures = {}, []
for transport in ('tcp', 'rdma'):
    p = art / transport
    d = json.loads((p / 'b6-result-slow.json').read_text())
    m = d['modes']['slow']
    if d['verdict'] != 'PASS' or d['cleanup_errors'] or m['refused_files'] or m['acknowledged'] != 22:
        failures.append(transport + ': B6 exactness/cleanup receipt')
    k = (p / 'k35.log').read_text()
    if 'NOTRUN' in k or 'Traceback' in k or 'PASS (' not in k:
        failures.append(transport + ': K35 receipt')
    roots = [Path(d['work']), Path(re.search(r'^kept (.+)$', k, re.M)[1])]
    totals = dict(connections=0, measured=0, app_tx=0, app_rx=0, tcp_tx=0, tcp_rx=0, violations=0, peer_fin=0)
    states, bad = {}, []
    for kind, root in zip(('b6', 'k35'), roots):
        for f in sorted(root.rglob('*.log')):
            dest = p / 'fixture-logs' / kind / f.relative_to(root)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, dest)
            with f.open(errors='replace') as stream:
                for line in stream:
                    if 'RDMA counters qpn=' not in line:
                        continue
                    fields = {}
                    for key, pattern in (
                        ('measured', r'tcp_measured=(\d+)'),
                        ('app_tx', r'app_tx=\d+/(\d+)'), ('app_rx', r'app_rx=\d+/(\d+)'),
                        ('tcp_tx', r'post_setup_tcp=(\d+)/'), ('tcp_rx', r'post_setup_tcp=\d+/(\d+)'),
                        ('violations', r'violations=(\d+)'), ('peer_fin', r'peer_fin=(\d+)'),
                        ('state', r'tcp_state=(\d+)')):
                        match = re.search(pattern, line)
                        fields[key] = int(match[1]) if match else -1
                    totals['connections'] += 1
                    for key in totals:
                        if key != 'connections':
                            totals[key] += fields[key]
                    state = str(fields['state'])
                    states[state] = states.get(state, 0) + 1
                    if (fields['measured'] != 1 or fields['tcp_tx'] != 0 or fields['tcp_rx'] != 0 or
                            fields['violations'] != 0 or fields['peer_fin'] not in (0, 1) or fields['state'] < 0):
                        bad.append(dict(file=str(f), line=line.rstrip()))
    if transport == 'rdma':
        if not totals['connections'] or totals['app_tx'] <= 0 or totals['app_rx'] <= 0 or bad:
            failures.append('rdma: strict measured zero-application-TCP counters')
    elif totals['connections']:
        failures.append('tcp: unexpected RDMA connection')
    report[transport] = dict(totals=totals, states=states, bad=bad, roots=[str(r) for r in roots])
for f in art.rglob('*-exit'):
    if f.read_text().strip() != '0':
        failures.append(str(f) + ': nonzero exit')
negative = (art / 'native-tcp-violation.log').read_text()
if not re.search(r'violations=[1-9]\d*', negative) or 'PASS:' not in negative:
    failures.append('intentional one-byte TCP violation not detected')
records = [json.loads(x) for x in (art / 'ownership.jsonl').read_text().splitlines()]
foreign = [r for r in records if r['foreign_jobs']]
if not records or foreign:
    failures.append('ownership coverage')
report['ownership'] = dict(samples=len(records), foreign=foreign,
                           background='pre-existing real client explicitly recorded; not quiet host')
report['failures'] = failures
report['verdict'] = 'FAIL' if failures else 'PASS'
(art / 'strict-audit.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
raise SystemExit(bool(failures))
