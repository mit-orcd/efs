#!/usr/bin/env python3
"""Summarize AO raw results without changing their verdict or hiding failures."""
import argparse
import json
from pathlib import Path
import re
import statistics


def spread(values):
    return dict(median=statistics.median(values), minimum=min(values), maximum=max(values))


def summarize(root):
    result=json.loads((root/'result.json').read_text())
    rows=result['measurements']; groups={}
    for row in rows:
        groups.setdefault((row['size'],row['method']),[]).append(row)
    report=dict(verdict=result['verdict'], completed_cases=len(rows),
                errors=result.get('errors',[]), error=result.get('error'),
                cleanup_error=result.get('cleanup_error'), listings=[])
    for (size,method),measurements in sorted(groups.items()):
        entry=dict(size=size,method=method,repetitions=len(measurements))
        for metric in ('wall_s','rpc_sends','page_p99_us','cluster_read_round_starts',
                       'write_ops_s','listing_process_cpu_s','stat_p50_s','stat_p99_s'):
            values=[r[metric] for r in measurements if r.get(metric) is not None]
            if values:entry[metric]=spread(values)
        entry['cpu_s']={node:spread([r['cpu_s'][node] for r in measurements])
                        for node in measurements[0]['cpu_s']}
        entry['name_oracle_errors']=sum(bool(r['missing'] or r['extra'] or r['duplicates'] or r['rc']) for r in measurements)
        report['listings'].append(entry)
    report['raft_observation_high_water_us']={}
    for log in sorted(root.glob('n*.log')):
        peaks={}
        with log.open(errors='replace') as source:
            for line in source:
                if 'raft-obs:' not in line:continue
                for key,value in re.findall(r'(pump_hold_max|lock_wait_max|drain_max|tick_max|apply_max|persist_max)=(\d+)us',line):
                    peaks[key]=max(peaks.get(key,0),int(value))
        report['raft_observation_high_water_us'][log.name]=peaks
    report['limits']=['Read-round starts include background traffic; not listing-owned rounds.',
                      'Per-daemon CPU uses clock ticks; short windows may quantize to zero.',
                      'Raft maxima cover the entire fixture lifetime, including population.',
                      'Only the final result after payload checks and shutdown can qualify PASS.']
    for key in ('d4_counts','d4_errors','stat_p50_s','stat_p99_s'):
        if key in result:report[key]=result[key]
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('evidence',type=Path)
    args=parser.parse_args()
    print(json.dumps(summarize(args.evidence),indent=2))
