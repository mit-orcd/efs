#!/usr/bin/env python3
"""Summarize verified written-ring cells and qualified physical trace windows."""
import argparse, json, statistics
from pathlib import Path
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('root', type=Path)
a = p.parse_args()
traces = json.loads((a.root / 'block-summary.json').read_text())
result = []
for cell in sorted(a.root.glob('*-*')):
    if not cell.is_dir() or not (cell / 'final.json').exists():
        continue
    final = json.loads((cell / 'final.json').read_text())
    assert final['verified'] and final['mode'] == 'written-ring'
    assert len(final['writers']) == 3
    assert all(v['reopened_exact_blocks'] == 4096 for v in final['writers'])
    rows = [[json.loads(line) for line in path.read_text().splitlines()] for path in sorted(cell.glob('writer[0-2].jsonl'))]
    assert len(rows) == 3 and all(rows)
    windows = {}
    for label, index in (('first', 0), ('last', -1)):
        selected = [r[index] for r in rows]
        windows[label] = dict(total_operations_s=sum(v['operations_s'] for v in selected),
                             mean_of_writer_sync_ms=statistics.mean(v['sync_mean_ms'] for v in selected),
                             window_end_seconds=[v['seconds'] for v in selected])
    qualified = []
    for trace in traces:
        if Path(trace['path']).parent.name != cell.name:
            continue
        qualified.append(dict(phase=Path(trace['path']).name.split('-')[0],
                              usable=trace['usable_for_latency'],
                              unambiguous_pairs_per_issue=trace['unambiguous_pairs_per_issue'],
                              by_issued_rwbs=trace['by_issued_rwbs'],
                              errors=trace['error_completions'], lost=trace['lost_record_lines'],
                              counters=trace['counters']))
    result.append(dict(cell=cell.name, seconds=final['seconds'], windows=windows,
                       total_operations_s=final['total_operations_s'],
                       exact_reopened_blocks=sum(v['reopened_exact_blocks'] for v in final['writers']),
                       cpu_user_seconds=final['cpu_user_seconds'], cpu_system_seconds=final['cpu_system_seconds'],
                       traces=qualified))
complete = [r['cell'] for r in result] == ['0-nvme', '1-sata', '2-sata', '3-nvme'] and (a.root / 'run-exit.txt').exists() and (a.root / 'run-exit.txt').read_text().strip() == '0'
report = dict(cells=result, matrix_complete=complete, scope='Instrumented file-level controls, not EFS workload or hardware-ceiling acceptance. First/last are completed ~30-second writer windows; sync is arithmetic mean of three writer means. Physical distributions omit ambiguous pairs; all-device traces may include unrelated I/O and their own output writes. Close/reopen checks are not power-loss gates.')
(a.root / 'drive-summary.json').write_text(json.dumps(report, indent=2) + '\n')
for row in result:
    first, last = row['windows']['first'], row['windows']['last']
    print(row['cell'], 'ops/s', round(first['total_operations_s'], 1), '->', round(last['total_operations_s'], 1),
          'sync ms', round(first['mean_of_writer_sync_ms'], 3), '->', round(last['mean_of_writer_sync_ms'], 3),
          'exact blocks', row['exact_reopened_blocks'])
