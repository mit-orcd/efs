#!/usr/bin/env python3
"""Summarize one scripts/soak.py output directory as Markdown tables.

Reads only result.json, timeline.jsonl, plateau.json and logs/*.log under OUT;
writes nothing. Tables: lifecycle events, counters, operation latency
(outside/inside churn), per-window RSS assessment, packer progress per log
and the final verification/orphan state.

    python3 scripts/soak_tables.py ~/efs-artifacts/<name>/endurance
"""
import collections
import json
import re
import statistics
import sys
from pathlib import Path


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs)-1, int(q*len(xs)))] if xs else 0


def table(head, rows):
    out = ['| ' + ' | '.join(head) + ' |', '|' + ' --- |'*len(head)]
    out += ['| ' + ' | '.join(str(c) for c in r) + ' |' for r in rows]
    return '\n'.join(out)


def main(out):
    out = Path(out)
    result = json.loads((out/'result.json').read_text())
    rows = [json.loads(l) for l in (out/'timeline.jsonl').read_text().splitlines() if l.strip()]
    print(f"verdict {result['verdict']}, {result['seconds']:.1f} s total, source {result['source']}")
    print(f"reason: {result['reason'].strip().splitlines()[-1]}\n")

    events = result.get('completed_events') or []
    print(table(('#', 'kind', 'victim', 'scheduled s', 'actual s', 'duration s'),
                [(i+1, e['kind'], e['victim'], e['at'], f"{e['actual_at']:.1f}", f"{e['seconds']:.2f}")
                 for i, e in enumerate(events)]))
    by = collections.defaultdict(list)
    for e in events:
        by[e['kind']].append(e['seconds'])
    print()
    print(table(('event kind', 'count', 'min s', 'median s', 'max s'),
                [(k, len(v), f'{min(v):.2f}', f'{statistics.median(v):.2f}', f'{max(v):.2f}')
                 for k, v in sorted(by.items())]))

    kinds = collections.Counter(r['kind'] for r in rows)
    verified = [r for r in rows if r['kind'] == 'verified']
    status = [r for r in rows if r['kind'] == 'status']
    bad = [r for r in status if not (r.get('protection') or {}).get('full')]
    spans, begin = [], None
    for r in rows:
        if r['kind'] == 'churn-start':
            begin = r['t']
        elif r['kind'] == 'churn-end':
            spans.append((begin, r['t']))
            begin = None
    if begin is not None:
        spans.append((begin, float('inf')))  # run stopped inside this event
    bad_steady = [r for r in bad if not any(a <= r['t'] <= b for a, b in spans)]
    revoked = [r for r in rows if r['kind'] == 'crashed-session-revoked']
    print()
    print(table(('counter', 'value'), [
        ('acknowledged file fsyncs', kinds['ack-fsync']),
        ('acknowledged append fsyncs', kinds['ack-append-fsync']),
        ('bounded child commands', kinds['command']),
        ('timeouts', kinds['timeout']),
        ('exact verifications (both mounts)', len(verified)),
        ('status samples / not fully protected / of those outside churn', f'{len(status)} / {len(bad)} / {len(bad_steady)}'),
        ('crashed sessions revoked (resumed)', f"{len(revoked)} ({sum(1 for r in revoked if r['proof'].get('resumed'))})"),
        ('cleanup refusals', kinds['cleanup-refusal']),
        ('query not-indexed answers (bounded wait)', kinds['query-not-indexed']),
        ('RSS samples', kinds['rss']),
    ]))

    lat = collections.defaultdict(lambda: ([], []))
    for r in rows:
        if r['kind'] == 'command':
            lat[r['label']][1 if r['churn_at_start'] or r['churn_at_end'] else 0].append(r['seconds'])
    print()
    rws = []
    for label, (calm, churn) in sorted(lat.items()):
        for name, xs in (('steady', calm), ('churn', churn)):
            if xs:
                rws.append((label, name, len(xs), f'{pct(xs,.5):.3f}', f'{pct(xs,.99):.3f}', f'{max(xs):.3f}'))
    print(table(('operation', 'phase', 'count', 'p50 s', 'p99 s', 'max s'), rws))

    if (out/'plateau.json').exists():
        plateau = json.loads((out/'plateau.json').read_text())
    else:
        # Run stopped before its own assessment: apply the same per-window
        # rule offline to the retained samples; roles were not recorded.
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import rss_plateau as RSS
        w = [dict(path=p.name, pid=pid, role='(unrecorded)', **RSS.assess_points(pts))
             for p in sorted(out.glob('rss-quiet-*.txt')) for pid, pts in RSS.from_ps(p).items()]
        plateau = dict(verdict='FAIL' if any(r['verdict'] == 'FAIL' for r in w) else 'NOTRUN',
                       missing_roles='role coverage unknown (no plateau.json: run stopped first; offline assessment)',
                       method='offline: 120s warmup, >=360s measured, last-third growth <=5% of peak', windows=w)
    print(f"\nRSS {plateau['verdict']}, missing roles {plateau['missing_roles']}; {plateau['method']}\n")
    w = plateau['windows']
    print(table(('window', 'role', 'pid', 'verdict', 'measured s', 'first MiB', 'peak MiB', 'last MiB', 'last-third growth %'),
                [(r['path'], r.get('role', '-'), r.get('pid', '-'), r['verdict'],
                  f"{r['seconds']:.0f}" if 'seconds' in r else '-',
                  *(f"{r[k]/1024:.1f}" if k in r else '-' for k in ('first', 'peak', 'last')),
                  f"{r['growth_pct']:.2f}" if 'growth_pct' in r else '-')
                 for r in w if r['verdict'] != 'NOTRUN']))
    print(f"\n{sum(1 for r in w if r['verdict']=='NOTRUN')} short window/process rows NOTRUN (plateau.json or rss-quiet-*.txt keep them)")

    print()
    prow = []
    seg = re.compile(r'packer: segments group=(\d+) .*?total_kib=(\d+) live_kib=(\d+) dead_kib=(\d+).*?compacted_total=(\d+)')
    pas = re.compile(r'packer: group=(\d+) term=(\d+) .*?candidates=(\d+) eligible=(\d+) .*?packed_total=(\d+)')
    for log in sorted((out/'logs').glob('n*.log')):
        last = {}
        for line in log.read_text(errors='replace').splitlines():
            m = pas.search(line)
            if m:
                g = m.group(1)
                d = last.setdefault(g, {})
                d['packed'] = max(d.get('packed', 0), int(m.group(5)))
                d['cand'] = m.group(3)
                d['terms'] = d.get('terms', set()) | {m.group(2)}
            m = seg.search(line)
            if m:
                last.setdefault(m.group(1), {})['seg'] = m.groups()[1:]
        for g, d in sorted(last.items()):
            s = d.get('seg', ('-',)*4)
            prow.append((log.stem, g, ','.join(sorted(d.get('terms', ()), key=int)) or '-', d.get('packed', 0),
                         d.get('cand', '-'), *s))
    print(table(('log', 'group', 'leader terms', 'max packed_total', 'last candidates',
                 'segment total KiB', 'live KiB', 'dead KiB', 'compacted_total'), prow))

    final = [r for r in status if r['t'] >= (verified[-2]['t'] if len(verified) > 1 else 0)]
    print()
    print(table(('final check', 'value'), [
        ('last verification', f"{verified[-1]['files']} files, {verified[-1]['appends']} appends, {verified[-1]['mounts']} mounts" if verified else 'none'),
        ('last protection sample', json.dumps(status[-1]['protection']) if status else 'none'),
        ('orphans in samples after final revoke', sorted({(r.get('protection') or {}).get('orphans') for r in final}, key=str)),
    ]))


if __name__ == '__main__':
    main(sys.argv[1])
