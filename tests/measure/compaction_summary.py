#!/usr/bin/env python3
"""Reduce retained compaction_pressure.py evidence; never turn missing data into zero."""
import argparse
import json
from pathlib import Path
import re
p=argparse.ArgumentParser(description=__doc__);p.add_argument('work',type=Path);a=p.parse_args()
rows=[json.loads(line) for line in (a.work/'samples.jsonl').read_text().splitlines()]
assert rows and {r['node'] for r in rows}=={1,2,3}
result={}
for node in (1,2,3):
    samples=[r for r in rows if r['node']==node]
    def peak(fn):
        values=[v for r in samples if (v:=fn(r)) is not None]
        return max(values) if values else None
    result[f'node{node}']={
        'peak_rss_kb':peak(lambda r:r['rss_kb']),
        'peak_l0_bytes':peak(lambda r:(r['kv'] or {}).get('l0_bytes')),
        'peak_mt_bytes':peak(lambda r:(r['kv'] or {}).get('mt_bytes')),
        'peak_replication_lag':peak(lambda r:r['replication_lag']),
        'peak_commit_apply_lag':peak(lambda r:max(g['commit']-g['applied'] for g in r['groups'])),
        'peak_kv_physical_bytes':peak(lambda r:(r.get('disk') or {}).get('kv_physical_bytes')),
        'peak_log_physical_bytes':peak(lambda r:(r.get('disk') or {}).get('log_physical_bytes')),
        **{f'peak_{key}':peak(lambda r,k=key:(r['pressure'] or {}).get(k)) for key in
           ('pending_n','pending_bytes','log_bytes','proposal_n','proposal_bytes','inbox_hi','outbox_hi','recv_rejected')},
        'max_sampled_pump_hold_us':peak(lambda r:(r.get('timing') or {}).get('pump_hold_max')),
    }
ack=[tuple(map(int,match)) for match in re.findall(r'ack seq=(\d+) index=(\d+) latency_us=(\d+) retries=(\d+)',(a.work/'acks.log').read_text())]
assert ack and [r[0] for r in ack]==list(range(len(ack)))
lat=sorted(r[2] for r in ack)
result['acknowledged']={'count':len(ack),'bytes_each':1<<20,'p50_us':lat[len(lat)//2],
                        'p99_us':lat[min(len(lat)-1,len(lat)*99//100)],'max_us':max(lat),'max_retries':max(r[3] for r in ack)}
assert max(lat)<15_000_000,'ack latency exceeded explicit 15 second fixture gate'
held=[r['ts'] for r in rows if r['phase']=='held-cap'];recover=[r['ts'] for r in rows if r['phase']=='recover']
result['held_cap_seconds']=max(held)-min(held)
result['sampled_recovery_seconds']=max(recover)-min(recover)
fuse=a.work/'fuse-acks.jsonl'
if fuse.exists():
    fs=[json.loads(line) for line in fuse.read_text().splitlines()];assert len(fs)==32
    result['fuse']={'files':len(fs),'bytes_each':256<<10,'max_fsync_us':max(r['fsync_us'] for r in fs)}
result['snapshot_import_observed']='raft-snap: import diff group=0 rc=0' in (a.work/'n3.log').read_text()
assert result['snapshot_import_observed']
result['cold_value_oracles']=(a.work/'acks.log').read_text().count('all 16 exact acknowledged pressure values survive cold KV reopen PASS')
assert result['cold_value_oracles']==3
result['scope']='Finite unchanged-cap fault experiment; sampled peaks are not universal process or permanent-stall throughput guarantees.'
print(json.dumps(result,indent=2))
