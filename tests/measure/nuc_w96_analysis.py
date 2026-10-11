#!/usr/bin/env python3
"""Reduce retained W96 ABBA results without hiding percentile tradeoffs."""
import json,statistics,sys
from pathlib import Path
root=Path(sys.argv[1]);out={}
for drive in ('sata','nvme'):
    for batch in (1,16):
        groups={v:[json.loads(p.read_text()) for p in sorted(root.glob(f'{drive}-batch{batch}-*-{v}.json'))] for v in ('control','reserve')}
        assert all(len(rows)==2 and all(r['replay_verified'] for r in rows) for rows in groups.values())
        metrics={v:dict(mean_records_s=statistics.mean(r['records_s'] for r in rows),pooled_sync_ms=sum(r['sync_mean_ms']*r['sync_calls_including_reopen'] for r in rows)/sum(r['sync_calls_including_reopen'] for r in rows),p50_us=[r['sync_p50_us'] for r in rows],p95_us=[r['sync_p95_us'] for r in rows],p99_us=[r['sync_p99_us'] for r in rows],max_ms=[r['sync_max_ms'] for r in rows],resets=[r['resets'] for r in rows],max_reserved_slack=max(r['max_reserved_slack'] for r in rows),max_allocated_bytes=max(r['max_allocated_bytes'] for r in rows)) for v,rows in groups.items()}
        metrics['throughput_gain_percent']=(metrics['reserve']['mean_records_s']/metrics['control']['mean_records_s']-1)*100
        out[f'{drive}-batch{batch}']=metrics
(root/'summary.json').write_text(json.dumps(out,indent=2))
print(json.dumps(out,indent=2))
