#!/usr/bin/env python3
"""Reconcile retained AMD W95 raw logs and JSON, without running new timings."""
import itertools
import json
from pathlib import Path
import re
import statistics
import sys
root=Path(__file__).resolve().parents[1]/'20261008-w93-w96-review/w95-churn'
rows=json.loads((root/'results.json').read_text())
assert len(rows)==24
expected=set(itertools.product(range(3),(1,32),(0,1),('files','indexed')))
seen=set()
for row in rows:
    key=(int(row['rep']),int(row['cohort']),int(row['mixed']),row['model'])
    assert key in expected and key not in seen,key
    seen.add(key)
    lines=(root/row['log']).read_text().splitlines()
    assert len(lines)==1 and lines[0].startswith('CHURN_OK '),row['log']
    raw=dict(re.findall(r'(\w+)=([^\s]+)',lines[0]))
    for field,value in raw.items():assert row[field]==value,(key,field)
    assert int(row['verified'])==128 and int(row['ops'])==7680 and int(row['rounds'])==60
    assert int(row['peak_allocated'])>=int(row['allocated'])
    if row['model']=='indexed':assert int(row['index_written'])==10320*7680//int(row['cohort'])
assert seen==expected
summary=[]
for cohort,mixed in itertools.product((1,32),(0,1)):
    selected=[row for row in rows if int(row['cohort'])==cohort and int(row['mixed'])==mixed]
    values={}
    for model in ('files','indexed'):
        group=[row for row in selected if row['model']==model]
        values[model]={
            'mean_ops_s':statistics.mean(float(row['ops_s']) for row in group),
            'mean_sample_p99_us':statistics.mean(float(row['p99_us']) for row in group),
            'max_peak_allocated_bytes':max(int(row['peak_allocated']) for row in group),
            'mean_body_written_per_op':statistics.mean(int(row['body_written'])/int(row['ops']) for row in group),
            'mean_index_written_per_op':statistics.mean(int(row['index_written'])/int(row['ops']) for row in group),
            'mean_reclaimed_allocated_bytes':statistics.mean(int(row['reclaimed']) for row in group),
        }
    ratios=[]
    for rep in range(3):
        pair={row['model']:float(row['ops_s']) for row in selected if int(row['rep'])==rep}
        ratios.append(pair['indexed']/pair['files'])
    summary.append({'cohort':cohort,'mixed':mixed,'models':values,
                    'paired_indexed_files_ops_ratio':ratios})
result={'samples':24,'operations':sum(int(row['ops']) for row in rows),
        'all_raw_fields_match':True,'all_expected_cells_present':True,
        'index_snapshot_bytes':10320,'summary':summary,
        'limits':['serial independent model','buffered single AMD filesystem',
                  'sampled allocation excludes transient manifest replacement',
                  'GC/inventory included in throughput but excluded from publication latency',
                  'process crash not hardware power loss','not production EFS or a selected format']}
output=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).with_name('w95-reviewed.json')
output.write_text(json.dumps(result,indent=2)+'\n')
print('24 samples / 184320 operations reconciled; full index bytes match all publication cohorts')
