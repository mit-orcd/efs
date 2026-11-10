#!/usr/bin/env python3
"""d4_backlog_table.py GATE_DIR... : pack backlog every 60 s with the create and pack rate of each minute, and the drain.

Reads the d4-samples.json that scripts/d4_progress.py writes in each d4-progress gate
directory (one sample per poll: t, phase, created, pack.backlog per group,
pack.files_packed) and prints one Markdown table row per cell plus the backlog series.
"""
import json, sys
from pathlib import Path


def summarize(gate_dir):
    s = json.loads((Path(gate_dir) / 'd4-samples.json').read_text())
    load = [x for x in s if x.get('phase') == 'load']
    if len(load) < 2:
        raise SystemExit(f'{gate_dir}: fewer than two load samples')
    backlog = lambda x: sum((x.get('pack') or {}).get('backlog', {}).values())
    a, b = load[0], load[-1]
    secs = b['t'] - a['t']
    created = b.get('created', 0) - a.get('created', 0)
    packed = (b['pack'].get('files_packed') or 0) - (a['pack'].get('files_packed') or 0)
    marks, nxt = [], a['t']
    for x in load:
        if x['t'] >= nxt:
            marks.append(x)
            nxt += 60
    if marks[-1] is not b:
        marks.append(b)
    # (seconds into the load, backlog, creates/s and packs/s since the previous mark)
    series = [(round(m['t'] - a['t']), backlog(m),
               round((m.get('created', 0) - p.get('created', 0)) / (m['t'] - p['t'])) if p else None,
               round(((m['pack'].get('files_packed') or 0) - (p['pack'].get('files_packed') or 0)) / (m['t'] - p['t'])) if p else None)
              for p, m in zip([None] + marks[:-1], marks)]
    after = [x for x in s if x['t'] > b['t']]
    drained = next((x['t'] - b['t'] for x in after if backlog(x) == 0), None)
    return dict(load_s=round(secs), creates_per_s=round(created / secs), packs_per_s=round(packed / secs),
                peak=max(backlog(x) for x in s), series=series,
                drain_s=round(drained, 1) if drained is not None else None)


def main(dirs):
    for d in dirs:
        r = summarize(d)
        print(f'| {d} | {r["load_s"]} | {r["creates_per_s"]} | {r["packs_per_s"]} | {r["peak"]} | {r["drain_s"]} |')
        print('  | s into load | backlog | creates/s | packs/s |')
        for t, v, c, k in r['series']:
            print(f'  | {t} | {v} | {c if c is not None else "-"} | {k if k is not None else "-"} |')


if __name__ == '__main__':
    if sys.argv[1:] == ['--self-test']:
        import tempfile
        with tempfile.TemporaryDirectory() as t:
            rows = [dict(t=0, phase='before', created=0, pack=dict(backlog={'0': 0}, files_packed=0))]
            rows += [dict(t=i, phase='load', created=10 * i, pack=dict(backlog={'0': 4 * i, '2': i}, files_packed=5 * i))
                     for i in range(0, 121)]
            rows += [dict(t=130, phase='drain', created=1200, pack=dict(backlog={'0': 0, '2': 0}, files_packed=1200))]
            Path(t, 'd4-samples.json').write_text(json.dumps(rows))
            r = summarize(t)
            assert r['creates_per_s'] == 10 and r['packs_per_s'] == 5 and r['peak'] == 600, r
            assert r['series'][:3] == [(0, 0, None, None), (60, 300, 10, 5), (120, 600, 10, 5)] and r['drain_s'] == 10, r
        print('self-test ok')
    else:
        main(sys.argv[1:])
