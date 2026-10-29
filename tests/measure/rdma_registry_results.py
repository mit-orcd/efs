#!/usr/bin/env python3
"""Summarize closed Task AI matrix rows; never turn partial evidence into PASS.

Run after timings finish. Output is raw evidence outside Git, not a release
qualification decision. Counter spans include startup/background/shutdown;
native/probe overhead and the churn diagnostic must be reviewed separately.
"""
import argparse
import json
import math
from pathlib import Path
import re
import statistics
import rdma_registry_probe as probe


def percentile(values, fraction):
    if not values: return None
    return sorted(values)[max(0,math.ceil(len(values)*fraction)-1)]


def spread(values):
    return dict(n=len(values),median=statistics.median(values),minimum=min(values),maximum=max(values))


def matrix_results(root):
    state=json.loads((root/'state.json').read_text()); rows=[]; grouped={}; diagnostics=[]
    names=[e['name'] for e in state['cells']]
    if len(names)!=len(set(names)): raise ValueError('duplicate matrix cell')
    for entry in state['cells']:
        path=root/entry['name']; result=json.loads((path/'result.json').read_text())
        if result['verdict']!='PASS_SCOPED': raise ValueError('nonpassing closed cell: '+str(path))
        if result['observed_foreign_work']: raise ValueError('timing overlap: '+str(path))
        label,streams,repeat=entry['name'].rsplit('-',2)
        row=dict(cell=entry['name'],label=label,streams=int(streams),repeat=int(repeat[1:]),
                 stat_p50_s=percentile(result['stat_syscall_s'],.50),
                 stat_p99_s=percentile(result['stat_syscall_s'],.99),
                 cpu_seconds=result['cpu_seconds'],elapsed_s=result['work_elapsed_s'])
        fsync=[x for w in result['rewrites'] for x in w['fsync_syscall_s']]
        row.update(rewrite_p50_s=percentile(fsync,.5),rewrite_p95_s=percentile(fsync,.95),
                   rewrite_p99_s=percentile(fsync,.99),rewrite_operations=len(fsync))
        if {(i['geometry'],i['mode']) for i in result['ior']}!={
                (g,m) for g in ('easy','hard') for m in ('write','verify')} or len(result['ior'])!=4:
            raise ValueError('incomplete/duplicate IOR phases')
        for item in result['ior']:
            text=(path/f"ior-{item['geometry']}-{item['mode']}.log").read_text(errors='replace')
            section=text.split('Results:',1)[1].split('Summary of all tests:',1)[0]
            matches=re.findall(r'^(write|read)\s+([0-9.]+)\s+',section,re.M)
            if len(matches)!=1: raise ValueError('ambiguous IOR result '+str(path))
            row[f"ior_{item['geometry']}_{item['mode']}_mib_s"]=float(matches[0][1])
        rows.append(row)
        for key,value in row.items():
            if key.startswith(('ior_','stat_','rewrite_p')) and isinstance(value,(int,float)):
                grouped.setdefault((label,int(streams),key),[]).append(value)
        if label=='probe-rdma':
            for filename in ('n1.log','n2.log','n3.log','mnt.log'):
                log=path/'cluster'/filename
                with log.open(errors='replace') as f: checked=probe.summarize(f)
                if checked['verdict']!='DIAGNOSTIC_ONLY':
                    raise ValueError('incomplete diagnostic: '+str(log))
                for line in log.open(errors='replace'):
                    if 'AI_REGISTRY ' in line:
                        d=json.loads(line.split('AI_REGISTRY ',1)[1])
                        if d['incomplete']: raise ValueError('incomplete registry record '+str(log))
                        span=d['last_ns']-d['first_ns']
                        if span<=0 or not d['calls']: raise ValueError('invalid registry interval')
                        d.update(cell=entry['name'],log=log.name,span_ns=span,
                                 acquisition_interval_pct=100*d['wait_ns']/span,
                                 mean_wait_ns=d['wait_ns']/d['calls'],mean_hold_ns=d['hold_ns']/d['calls'],
                                 acquisitions_s=d['calls']*1e9/span)
                        diagnostics.append(d)
    groups=[dict(label=l,streams=s,metric=k,**spread(v)) for (l,s,k),v in sorted(grouped.items())]
    expected={f'{l}-{s}-r{r}' for l in ('native-tcp','native-rdma','probe-rdma')
              for s in (1,8,32) for r in (1,2,3)}
    complete=state['verdict']=='PASS_SCOPED' and set(names)==expected and all(g['n']==3 for g in groups)
    return dict(verdict='COMPLETE_SCOPED' if complete else 'INCOMPLETE',rows=rows,
                median_spread=groups,diagnostic_records=diagnostics,
                limits='Single-host bounded geometry; stat cache applies; acquisition intervals include clock overhead. '
                       'No optimization, multi-host, release, or NIC/kernel wake-latency acceptance inferred.')


def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('matrix',type=Path)
    p.add_argument('output',type=Path); a=p.parse_args()
    data=matrix_results(a.matrix)
    with a.output.open('x') as f: json.dump(data,f,indent=2); f.write('\n')


if __name__=='__main__': main()
